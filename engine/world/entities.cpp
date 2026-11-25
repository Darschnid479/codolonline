#include "entities.hpp"
#include "../platform/platform.hpp"

#include <cstdio>

namespace dragon::world
{
	namespace
	{
		bool parse_vec3(const std::string& s, math::Vec3& out)
		{
			float x = 0, y = 0, z = 0;
			const int n = std::sscanf(s.c_str(), "%f %f %f", &x, &y, &z);
			if (n >= 1)
			{
				out = { x, y, z };
				return true;
			}
			return false;
		}
	}

	std::string MapEntity::get(const std::string& key, const std::string& fallback) const
	{
		const auto it = kv.find(key);
		return it != kv.end() ? it->second : fallback;
	}

	void EntityWorld::clear()
	{
		entities_.clear();
		player_spawns_.clear();
		intermissions_.clear();
		pathnodes_.clear();
	}

	bool EntityWorld::load_from_zone(const codol::zone::Zone& zone)
	{
		clear();
		const auto addr = codol::zone::find_map_ents(zone);
		if (!addr) return false;
		codol::zone::MapEntsView view;
		if (!codol::zone::read_map_ents(zone, addr, view) || view.entityString.empty())
			return false;
		const bool ok = parse_entity_string(view.entityString);
		platform::log_info("Parsed MapEnts '%s': %zu entities, %zu spawns, %zu intermissions, %zu pathnodes",
			std::string(view.name).c_str(), entities_.size(), player_spawns_.size(),
			intermissions_.size(), pathnodes_.size());
		return ok;
	}

	bool EntityWorld::parse_entity_string(std::string_view text)
	{
		clear();
		std::size_t pos = 0;
		const std::size_t n = text.size();

		auto skip_ws = [&]() {
			while (pos < n && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n'))
				++pos;
		};

		auto read_quoted = [&](std::string& out) -> bool {
			skip_ws();
			if (pos >= n || text[pos] != '"') return false;
			++pos;
			const std::size_t start = pos;
			while (pos < n && text[pos] != '"') ++pos;
			out.assign(text.data() + start, pos - start);
			if (pos < n && text[pos] == '"') ++pos;
			return true;
		};

		while (pos < n)
		{
			skip_ws();
			if (pos >= n) break;
			if (text[pos] != '{')
			{
				++pos;
				continue;
			}
			++pos;
			MapEntity ent;
			while (pos < n)
			{
				skip_ws();
				if (pos >= n) break;
				if (text[pos] == '}') { ++pos; break; }
				std::string k, v;
				if (!read_quoted(k) || !read_quoted(v))
				{
					while (pos < n && text[pos] != '\n' && text[pos] != '}') ++pos;
					continue;
				}
				if (k == "classname") ent.classname = v;
				else if (k == "origin") ent.has_origin = parse_vec3(v, ent.origin);
				else if (k == "angles") ent.has_angles = parse_vec3(v, ent.angles);
				ent.kv.emplace(std::move(k), std::move(v));
			}

			if (!ent.classname.empty())
			{
				const std::string& cn = ent.classname;
				if (ent.has_origin)
				{
					if (cn == "mp_global_intermission")
					{
						// IW4 angles: [pitch, yaw, roll] where positive pitch tilts DOWN
						intermissions_.push_back({ ent.origin, ent.angles.y, -ent.angles.x, cn });
					}
					else if (cn.find("spawn") != std::string::npos || cn == "info_player_start")
					{
						player_spawns_.push_back({ ent.origin, ent.angles.y, -ent.angles.x, cn });
					}
					else if (cn.rfind("node_", 0) == 0)
					{
						pathnodes_.push_back(ent.origin);
					}
				}
				entities_.push_back(std::move(ent));
			}
		}
		return !entities_.empty();
	}

	SpawnPoint EntityWorld::choose_initial_spawn(const math::AABB& fallback_bounds) const
	{
		// Pass 1: Prefer genuine multiplayer player spawn points (in MP maps, info_player_start is often an unused reference marker)
		for (const auto& s : player_spawns_)
		{
			if (s.classname == "mp_tdm_spawn_allies_start" || s.classname == "mp_tdm_spawn_axis_start" ||
			    s.classname == "mp_tdm_spawn" || s.classname == "mp_dm_spawn")
			{
				return s;
			}
		}
		// Pass 2: Zombies / Coop / SP start points
		for (const auto& s : player_spawns_)
		{
			if (s.classname == "info_player_start")
				return s;
		}
		if (!player_spawns_.empty())
			return player_spawns_.front();
		if (!intermissions_.empty())
			return intermissions_.front();

		SpawnPoint sp;
		sp.classname = "fallback_center";
		if (fallback_bounds.valid())
		{
			sp.origin = fallback_bounds.center();
			sp.origin.z = fallback_bounds.mins.z + fallback_bounds.half_size().z * 0.5f;
		}
		else
		{
			sp.origin = { 0.0f, 0.0f, 64.0f };
		}
		return sp;
	}
}
