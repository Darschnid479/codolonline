#include "gameplay.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace dragon::world
{
	namespace
	{
		std::string trim_str(std::string_view sv)
		{
			std::size_t a = 0, b = sv.size();
			while (a < b && std::isspace(static_cast<unsigned char>(sv[a]))) ++a;
			while (b > a && std::isspace(static_cast<unsigned char>(sv[b - 1]))) --b;
			return std::string(sv.substr(a, b - a));
		}

		std::string lower_str(std::string s)
		{
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}
	}

	// =========================================================================
	// WeaponSystem
	// =========================================================================
	void WeaponSystem::load_from_zone(const codol::zone::Zone& zone)
	{
		weapons_.clear();
		active_idx_ = 0;
		impacts_.clear();

		// Discover actual ASSET_TYPE_WEAPON assets inside the loaded zone (header size 0xBFC)
		for (const auto& rec : zone.all_assets())
		{
			if (rec.type != codol::c1::ASSET_TYPE_WEAPON) continue;
			const std::string_view wname = zone.asset_name(rec.type, rec.header);
			if (wname.empty() || wname == "none" || wname == "defaultweapon_mp") continue;

			// Avoid duplicates
			bool dup = false;
			for (const auto& w : weapons_)
				if (w.internal_name == wname) { dup = true; break; }
			if (dup) continue;

			WeaponInfo info;
			info.internal_name = std::string(wname);
			info.display_name = info.internal_name;
			// Attempt to read szDisplayName pointer at offset 4 of WeaponCompleteDef
			const auto disp_ptr = zone.get<codol::zone::zaddr>(rec.header + 4);
			const auto disp = zone.c_string(disp_ptr);
			if (!disp.empty() && disp.size() < 64)
				info.display_name = std::string(disp);

			const std::string low = lower_str(info.internal_name);
			if (low.find("sniper") != std::string::npos || low.find("cheytac") != std::string::npos || low.find("barrett") != std::string::npos)
			{
				info.clip_size = 5; info.ammo_in_clip = 5; info.ammo_reserve = 30;
				info.fire_interval_sec = 0.85f; info.ads_fov = 22.0f; info.damage = 98;
			}
			else if (low.find("shotgun") != std::string::npos || low.find("spas") != std::string::npos || low.find("m1014") != std::string::npos)
			{
				info.clip_size = 8; info.ammo_in_clip = 8; info.ammo_reserve = 40;
				info.fire_interval_sec = 0.55f; info.ads_fov = 60.0f; info.damage = 75;
			}
			else if (low.find("pistol") != std::string::npos || low.find("usp") != std::string::npos || low.find("beretta") != std::string::npos || low.find("deserteagle") != std::string::npos)
			{
				info.clip_size = 12; info.ammo_in_clip = 12; info.ammo_reserve = 72;
				info.fire_interval_sec = 0.16f; info.ads_fov = 62.0f; info.damage = 38;
			}
			else
			{
				info.clip_size = 30; info.ammo_in_clip = 30; info.ammo_reserve = 150;
				info.fire_interval_sec = 0.09f; info.ads_fov = 52.0f; info.damage = 34;
			}
			weapons_.push_back(std::move(info));
			if (weapons_.size() >= 12) break;
		}

		// If the map zone relies on common_mp for weapons, record an unloaded placeholder slot explicitly
		if (weapons_.empty())
		{
			WeaponInfo w;
			w.internal_name = "zone_weapon_unloaded";
			w.display_name = "No weapon asset in map zone (load common_mp.ff)";
			w.clip_size = 30; w.ammo_in_clip = 30; w.ammo_reserve = 90;
			weapons_.push_back(std::move(w));
		}

		platform::log_info("WeaponSystem indexed %zu weapon assets from zone '%s'", weapons_.size(), zone.name().c_str());
	}

	const WeaponInfo& WeaponSystem::active_weapon() const
	{
		static const WeaponInfo kEmpty{};
		return weapons_.empty() ? kEmpty : weapons_[active_idx_ % weapons_.size()];
	}

	void WeaponSystem::update(const platform::InputState& input, float dt, const math::Vec3& eye, const math::Vec3& forward,
		const CollisionWorld* coll, std::vector<BotEntity>* bots)
	{
		if (weapons_.empty()) return;
		WeaponInfo& w = weapons_[active_idx_ % weapons_.size()];

		if (fire_cooldown_ > 0.0f) fire_cooldown_ = std::max(0.0f, fire_cooldown_ - dt);
		recoil_pitch_ = std::max(0.0f, recoil_pitch_ - dt * 14.0f);

		for (auto it = impacts_.begin(); it != impacts_.end();)
		{
			it->ttl -= dt;
			if (it->ttl <= 0.0f) it = impacts_.erase(it);
			else ++it;
		}

		// Weapon switch (Triangle)
		if (input.pressed(platform::BTN_TRIANGLE) && weapons_.size() > 1)
		{
			active_idx_ = (active_idx_ + 1) % weapons_.size();
			reload_timer_ = 0.0f;
			platform::audio_play_tone(520.0f, 0.04f, 0.06f);
			return;
		}

		// Reload timer
		if (reload_timer_ > 0.0f)
		{
			reload_timer_ -= dt;
			if (reload_timer_ <= 0.0f)
			{
				const int need = w.clip_size - w.ammo_in_clip;
				const int take = std::min(need, w.ammo_reserve);
				w.ammo_in_clip += take;
				w.ammo_reserve -= take;
				reload_timer_ = 0.0f;
				platform::audio_play_tone(640.0f, 0.05f, 0.07f);
			}
			return;
		}

		// Manual reload (Square)
		if (input.pressed(platform::BTN_SQUARE) && w.ammo_in_clip < w.clip_size && w.ammo_reserve > 0)
		{
			reload_timer_ = w.reload_time_sec;
			platform::audio_play_tone(310.0f, 0.06f, 0.07f);
			return;
		}

		// Fire (R2)
		const bool firing = input.r2 > 0.45f || input.down(platform::BTN_R2);
		if (firing && fire_cooldown_ <= 0.0f)
		{
			if (w.ammo_in_clip <= 0)
			{
				if (w.ammo_reserve > 0) reload_timer_ = w.reload_time_sec;
				platform::audio_play_tone(220.0f, 0.03f, 0.05f);
				fire_cooldown_ = 0.25f;
				return;
			}
			--w.ammo_in_clip;
			++shots_fired_;
			fire_cooldown_ = w.fire_interval_sec;
			recoil_pitch_ = std::min(5.0f, recoil_pitch_ + 1.35f);
			platform::audio_play_tone(125.0f, 0.045f, 0.14f);
			platform::input_set_rumble(140, 80);

			// Raycast against clipMap world geometry
			const float max_range = 4096.0f;
			math::Vec3 end = eye + forward * max_range;
			float best_dist = max_range;
			if (coll && coll->loaded())
			{
				const auto tr = coll->ray_trace(eye, end);
				if (tr.fraction < 1.0f)
				{
					best_dist = max_range * tr.fraction;
					impacts_.push_back({ tr.endpos, tr.normal, 1.5f });
					if (impacts_.size() > 32) impacts_.erase(impacts_.begin());
				}
			}

			// Ray-sphere/capsule hit test against active offline bots
			if (bots)
			{
				for (auto& bot : *bots)
				{
					if (!bot.alive) continue;
					const math::Vec3 bot_center = bot.origin + math::Vec3{ 0.0f, 0.0f, 38.0f };
					const math::Vec3 to_bot = bot_center - eye;
					const float proj = math::dot(to_bot, forward);
					if (proj <= 0.0f || proj >= best_dist) continue;
					const math::Vec3 closest = eye + forward * proj;
					if (math::length(closest - bot_center) <= 24.0f)
					{
						bot.health -= w.damage;
						++hits_registered_;
						platform::audio_play_tone(980.0f, 0.035f, 0.12f);
						if (bot.health <= 0)
						{
							bot.alive = false;
							bot.respawn_timer = 4.0f;
						}
						break;
					}
				}
			}
		}
		else if (!firing)
		{
			platform::input_set_rumble(0, 0);
		}
	}

	// =========================================================================
	// SkeletalRegistry
	// =========================================================================
	void SkeletalRegistry::index_zone(const codol::zone::Zone& zone)
	{
		anim_names_.clear();
		rigged_models_.clear();
		for (const auto& rec : zone.all_assets())
		{
			if (rec.type == codol::c1::ASSET_TYPE_XANIMPARTS)
			{
				codol::zone::AnimView av;
				if (codol::zone::read_anim(zone, rec.header, av) && !av.name.empty())
					anim_names_.emplace_back(av.name);
			}
			else if (rec.type == codol::c1::ASSET_TYPE_XMODEL)
			{
				codol::zone::ModelView mv;
				if (codol::zone::read_model(zone, rec.header, mv, false) && !mv.bones.empty())
					rigged_models_.emplace_back(mv.name);
			}
		}
		platform::log_info("SkeletalRegistry indexed %zu XAnimParts and %zu rigged XModels",
			anim_names_.size(), rigged_models_.size());
	}

	bool SkeletalRegistry::evaluate_bind_pose(const codol::zone::Zone& zone, const std::string& model_name, SkeletonPose& out) const
	{
		const auto addr = zone.find_asset(codol::c1::ASSET_TYPE_XMODEL, model_name);
		if (!addr) return false;
		codol::zone::ModelView mv;
		if (!codol::zone::read_model(zone, addr, mv, false)) return false;

		out = {};
		out.model_name = std::string(mv.name);
		out.bone_names.reserve(mv.bones.size());
		out.parent_indices.reserve(mv.bones.size());
		out.world_positions.reserve(mv.bones.size());
		for (const auto& b : mv.bones)
		{
			out.bone_names.emplace_back(b.name);
			out.parent_indices.push_back(b.parent);
			out.world_positions.push_back({ b.baseTrans[0], b.baseTrans[1], b.baseTrans[2] });
		}
		return !out.bone_names.empty();
	}

	// =========================================================================
	// Clean-Room GSC Runtime VM
	// =========================================================================
	void GscRuntime::reset()
	{
		scripts_.clear();
		threads_.clear();
		level_vars_.clear();
		executed_statements_ = 0;
	}

	bool GscRuntime::load_script_source(const std::string& script_name, std::string_view source)
	{
		// Strip // and /* */ comments
		std::string clean;
		clean.reserve(source.size());
		for (std::size_t i = 0; i < source.size();)
		{
			if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '/')
			{
				while (i < source.size() && source[i] != '\n') ++i;
			}
			else if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '*')
			{
				i += 2;
				while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/')) ++i;
				i += 2;
			}
			else
			{
				clean.push_back(source[i++]);
			}
		}

		// Parse top-level function definitions: name(params) { ... }
		std::vector<GscFunction> funcs;
		std::size_t pos = 0;
		const std::size_t n = clean.size();
		while (pos < n)
		{
			while (pos < n && std::isspace(static_cast<unsigned char>(clean[pos]))) ++pos;
			if (pos >= n) break;
			if (clean[pos] == '#')
			{
				while (pos < n && clean[pos] != '\n') ++pos;
				continue;
			}
			const std::size_t id_start = pos;
			while (pos < n && (std::isalnum(static_cast<unsigned char>(clean[pos])) || clean[pos] == '_')) ++pos;
			if (pos == id_start) { ++pos; continue; }
			const std::string fname = lower_str(clean.substr(id_start, pos - id_start));
			while (pos < n && std::isspace(static_cast<unsigned char>(clean[pos]))) ++pos;
			if (pos >= n || clean[pos] != '(') continue;
			// Skip parameter list
			int pdepth = 0;
			while (pos < n)
			{
				if (clean[pos] == '(') ++pdepth;
				else if (clean[pos] == ')') { --pdepth; if (pdepth == 0) { ++pos; break; } }
				++pos;
			}
			while (pos < n && std::isspace(static_cast<unsigned char>(clean[pos]))) ++pos;
			if (pos >= n || clean[pos] != '{') continue;
			++pos;
			const std::size_t body_start = pos;
			int bdepth = 1;
			while (pos < n && bdepth > 0)
			{
				if (clean[pos] == '{') ++bdepth;
				else if (clean[pos] == '}') --bdepth;
				++pos;
			}
			const std::size_t body_end = pos > 0 ? pos - 1 : body_start;
			const std::string_view body(clean.data() + body_start, body_end - body_start);

			GscFunction fn;
			fn.name = fname;
			std::size_t s0 = 0;
			for (std::size_t k = 0; k < body.size(); ++k)
			{
				if (body[k] == ';')
				{
					std::string st = trim_str(body.substr(s0, k - s0));
					if (!st.empty()) fn.statements.push_back(std::move(st));
					s0 = k + 1;
				}
			}
			funcs.push_back(std::move(fn));
		}

		if (funcs.empty()) return false;
		scripts_[lower_str(script_name)] = std::move(funcs);
		return true;
	}

	std::size_t GscRuntime::load_from_zone_and_assets(const codol::zone::Zone& zone, const assets::AssetManager& mgr, const std::string& map_name)
	{
		reset();
		std::size_t loaded = 0;

		// 1. Load .gsc rawfiles embedded inside the loaded FastFile zone
		for (const auto& rec : zone.all_assets())
		{
			if (rec.type != codol::c1::ASSET_TYPE_RAWFILE) continue;
			codol::zone::RawFileView rf;
			if (!codol::zone::read_rawfile(zone, rec.header, rf)) continue;
			if (rf.name.size() < 4 || rf.name.substr(rf.name.size() - 4) != ".gsc") continue;
			std::vector<std::uint8_t> bytes;
			if (codol::zone::rawfile_contents(rf, bytes) && !bytes.empty())
			{
				const std::string_view src(reinterpret_cast<const char*>(bytes.data()), bytes.size());
				if (load_script_source(std::string(rf.name), src))
					++loaded;
			}
		}

		// 2. Load level script from main/sh1x_svr_scripts.iwd or sh1x_rawfiles
		const std::vector<std::string> level_candidates = {
			"maps/mp/" + map_name + ".gsc",
			"maps/mp/gametypes/_globallogic.gsc"
		};
		for (const auto& path : level_candidates)
		{
			std::vector<std::uint8_t> bytes;
			if (mgr.read_script_file(path, bytes) && !bytes.empty())
			{
				const std::string_view src(reinterpret_cast<const char*>(bytes.data()), bytes.size());
				if (load_script_source(path, src))
					++loaded;
			}
		}

		// Spawn main() on level script if present
		const std::string map_script = lower_str("maps/mp/" + map_name + ".gsc");
		if (scripts_.count(map_script))
			spawn_thread(map_script, "main");

		platform::log_info("GscRuntime loaded %zu scripts (%zu functions) for '%s'",
			loaded, function_count(), map_name.c_str());
		return loaded;
	}

	void GscRuntime::spawn_thread(const std::string& script_name, const std::string& func_name)
	{
		const std::string sk = lower_str(script_name);
		const std::string fk = lower_str(func_name);
		auto it = scripts_.find(sk);
		if (it == scripts_.end()) return;
		for (const auto& fn : it->second)
		{
			if (fn.name == fk)
			{
				threads_.push_back({ sk, fk, 0, 0.0f, {}, false });
				return;
			}
		}
	}

	void GscRuntime::notify(const std::string& event_name)
	{
		const std::string ev = lower_str(event_name);
		for (auto& th : threads_)
		{
			if (!th.finished && !th.waiting_notify.empty() && th.waiting_notify == ev)
				th.waiting_notify.clear();
		}
	}

	void GscRuntime::exec_statement(GscThread& th, const std::string& stmt)
	{
		++executed_statements_;
		const std::string low = lower_str(stmt);

		// wait <seconds>
		if (low.rfind("wait ", 0) == 0 || low.rfind("wait(", 0) == 0)
		{
			const float sec = static_cast<float>(std::atof(stmt.c_str() + 4));
			th.wait_timer = std::max(0.05f, sec);
			return;
		}
		// waittill("event")
		if (const auto wp = low.find("waittill("); wp != std::string::npos)
		{
			const auto q1 = stmt.find('"', wp);
			const auto q2 = q1 != std::string::npos ? stmt.find('"', q1 + 1) : std::string::npos;
			if (q1 != std::string::npos && q2 != std::string::npos)
				th.waiting_notify = lower_str(stmt.substr(q1 + 1, q2 - q1 - 1));
			return;
		}
		// notify("event")
		if (const auto np = low.find("notify("); np != std::string::npos)
		{
			const auto q1 = stmt.find('"', np);
			const auto q2 = q1 != std::string::npos ? stmt.find('"', q1 + 1) : std::string::npos;
			if (q1 != std::string::npos && q2 != std::string::npos)
				notify(stmt.substr(q1 + 1, q2 - q1 - 1));
			return;
		}
		// setDvar("name", "value")
		if (const auto dp = low.find("setdvar("); dp != std::string::npos)
		{
			const auto q1 = stmt.find('"', dp);
			const auto q2 = q1 != std::string::npos ? stmt.find('"', q1 + 1) : std::string::npos;
			const auto q3 = q2 != std::string::npos ? stmt.find('"', q2 + 1) : std::string::npos;
			const auto q4 = q3 != std::string::npos ? stmt.find('"', q3 + 1) : std::string::npos;
			if (q1 != std::string::npos && q2 != std::string::npos && q3 != std::string::npos && q4 != std::string::npos)
			{
				platform::DebugConsole::instance().set_dvar(
					stmt.substr(q1 + 1, q2 - q1 - 1),
					stmt.substr(q3 + 1, q4 - q3 - 1));
			}
			return;
		}
		// level.<var> = <val>
		if (low.rfind("level.", 0) == 0)
		{
			const auto eq = stmt.find('=');
			if (eq != std::string::npos && (eq + 1 >= stmt.size() || stmt[eq + 1] != '='))
			{
				const std::string k = trim_str(stmt.substr(6, eq - 6));
				const std::string v = trim_str(stmt.substr(eq + 1));
				if (!k.empty()) level_vars_[k] = v;
			}
			return;
		}
		// thread func()
		if (low.rfind("thread ", 0) == 0)
		{
			const auto paren = stmt.find('(', 7);
			if (paren != std::string::npos)
			{
				const std::string fn = trim_str(stmt.substr(7, paren - 7));
				spawn_thread(th.script_name, fn);
			}
		}
	}

	void GscRuntime::tick(float dt)
	{
		const std::size_t count = threads_.size();
		for (std::size_t i = 0; i < count; ++i)
		{
			GscThread& th = threads_[i];
			if (th.finished) continue;
			if (!th.waiting_notify.empty()) continue;
			if (th.wait_timer > 0.0f)
			{
				th.wait_timer -= dt;
				if (th.wait_timer > 0.0f) continue;
			}

			auto it = scripts_.find(th.script_name);
			if (it == scripts_.end()) { th.finished = true; continue; }
			const GscFunction* fn = nullptr;
			for (const auto& f : it->second)
				if (f.name == th.func_name) { fn = &f; break; }
			if (!fn) { th.finished = true; continue; }

			int budget = 32;
			while (budget-- > 0 && th.pc < fn->statements.size() && th.wait_timer <= 0.0f && th.waiting_notify.empty())
			{
				exec_statement(th, fn->statements[th.pc++]);
			}
			if (th.pc >= fn->statements.size())
				th.finished = true;
		}
	}

	std::size_t GscRuntime::function_count() const
	{
		std::size_t total = 0;
		for (const auto& [k, v] : scripts_) total += v.size();
		return total;
	}

	std::size_t GscRuntime::active_thread_count() const
	{
		std::size_t n = 0;
		for (const auto& th : threads_) if (!th.finished) ++n;
		return n;
	}

	// =========================================================================
	// OfflineServer (Local Match Loop + Bot Waypoint Navigation)
	// =========================================================================
	void OfflineServer::start_match(const std::string& map_name, const std::string& gametype,
		const EntityWorld& ents, const codol::zone::Zone& zone, const assets::AssetManager& mgr)
	{
		map_name_ = map_name;
		gametype_ = gametype;
		wave_number_ = 1;
		player_score_ = 0;
		player_kills_ = 0;
		server_accum_ = 0.0f;
		waypoints_ = ents.pathnodes();
		spawns_ = ents.player_spawns();
		bots_.clear();

		gsc_.load_from_zone_and_assets(zone, mgr, map_name);

		// Spawn up to 6 local bots at authentic map spawn points
		const std::size_t bot_count = std::min<std::size_t>(6, spawns_.size());
		for (std::size_t i = 0; i < bot_count; ++i)
		{
			BotEntity b;
			b.id = static_cast<int>(i + 1);
			b.team = (gametype_ == "zm" || gametype_ == "survival") ? 1 : static_cast<int>(i % 2);
			b.name = (gametype_ == "zm") ? ("Cyborg_" + std::to_string(i + 1)) : ("Bot_" + std::to_string(i + 1));
			b.origin = spawns_[i].origin;
			b.yaw_deg = spawns_[i].yaw_deg;
			b.health = b.max_health = (gametype_ == "zm") ? 150 : 100;
			b.target_node = waypoints_.empty() ? 0 : (i * 7) % waypoints_.size();
			bots_.push_back(std::move(b));
		}

		platform::log_info("OfflineServer started '%s' (gametype=%s, %zu bots, %zu pathnodes)",
			map_name_.c_str(), gametype_.c_str(), bots_.size(), waypoints_.size());
	}

	void OfflineServer::tick(float dt, const math::Vec3& player_pos, const CollisionWorld* coll)
	{
		server_accum_ += dt;
		constexpr float SV_FRAME = 0.05f; // 20 Hz IW4/CODOL server tick rate
		if (server_accum_ < SV_FRAME) return;
		const float step = server_accum_;
		server_accum_ = 0.0f;

		gsc_.tick(step);

		int alive_enemies = 0;
		for (auto& b : bots_)
		{
			if (!b.alive)
			{
				b.respawn_timer -= step;
				if (b.respawn_timer <= 0.0f && !spawns_.empty())
				{
					++player_kills_;
					player_score_ += 100;
					const auto& sp = spawns_[static_cast<std::size_t>(b.id + player_kills_) % spawns_.size()];
					b.origin = sp.origin;
					b.health = b.max_health;
					b.alive = true;
				}
				continue;
			}
			++alive_enemies;

			// Target: Zombies/Cyborg chase player; MP bots patrol real MapEnts pathnodes
			math::Vec3 goal = player_pos;
			if ((gametype_ != "zm" && gametype_ != "survival") && !waypoints_.empty())
			{
				goal = waypoints_[b.target_node % waypoints_.size()];
				const math::Vec3 d = goal - b.origin;
				if (d.x * d.x + d.y * d.y < 36.0f * 36.0f)
					b.target_node = (b.target_node + 1) % waypoints_.size();
			}

			math::Vec3 dir{ goal.x - b.origin.x, goal.y - b.origin.y, 0.0f };
			const float dist = std::sqrt(dir.x * dir.x + dir.y * dir.y);
			if (dist > 8.0f)
			{
				dir.x /= dist;
				dir.y /= dist;
				b.yaw_deg = std::atan2(dir.y, dir.x) * math::RAD2DEG;
				const float bot_speed = (gametype_ == "zm") ? 135.0f : 155.0f;
				math::Vec3 next = b.origin + dir * (bot_speed * step);
				if (coll && coll->loaded())
				{
					const auto tr = coll->box_trace(b.origin + math::Vec3{ 0, 0, 18 }, next + math::Vec3{ 0, 0, 18 },
						{ -14, -14, 0 }, { 14, 14, 52 });
					if (tr.fraction > 0.1f)
						next = tr.endpos - math::Vec3{ 0, 0, 18 };
					else if (!waypoints_.empty())
						b.target_node = (b.target_node + 3) % waypoints_.size();
				}
				b.origin = next;
			}
		}
		if ((gametype_ == "zm" || gametype_ == "survival") && alive_enemies == 0 && !bots_.empty())
			++wave_number_;
	}
}
