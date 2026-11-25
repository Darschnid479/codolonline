// Project Dragon — Call of Duty Online 1.1.1.13 Native PS4 Port
// Entry point shared between native PlayStation 4 (OrbisOS x86-64 eboot.bin) and PC Debug builds.
// Normal startup follows the original CODOL 1.1.1.13 sequence:
//   1. Root splash.bmp (Sys_ShowSplashWindow @ 0x00632908)
//   2. Intro cinematics PUB_logo.bk2 / CODO_logo.bk2 (logged explicitly as Bink2 KB2i NOT_IMPLEMENTED)
//   3. Original IW loading menu ui_mp/load.txt -> MENU load (patch_code_post_gfx_mp.ff)
//   4. Original Scaleform loginView / logoView bitmaps (codologin.gfx / codo.gfx) over 3D ui_viewer_mp.ff
// The engine's self-drawn map selector and telemetry HUD are strictly explicit developer tools
// (--dev-map-menu, --dev-hud, --dev-boxes, or PS4 L1+R1+TOUCHPAD / console `devmapmenu`).

#include "../assets/asset_manager.hpp"
#include "../gfx/renderer.hpp"
#include "../platform/platform.hpp"
#include "../ui/authentic_ui.hpp"
#include "../world/bsp_world.hpp"
#include "../world/collision.hpp"
#include "../world/entities.hpp"
#include "../world/gameplay.hpp"
#include "../world/player_movement.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
	struct AppOptions
	{
		std::string game_dir;
		std::string initial_map;
		std::string gametype;
		std::string render_out_bmp;
		std::string force_stage; // "", "splash", "load", "login", "logo", "cyborgs_logo", "mapload", "ingame"
		int max_frames = 0;      // 0 = run until user quits (PS4 infinite loop)
		int width = 1280;
		int height = 720;
		int max_tex_dim = 256;
		float cam_yaw_offset = 0.0f;
		float cam_pitch_offset = 0.0f;
		float cam_z_offset = 0.0f;
		float load_progress = 0.65f;
		bool use_intermission_cam = false;
		bool use_lobby_3d = false;
		bool dev_map_menu = false;
		bool dev_hud = false;
		bool dev_crosshair = false;
		bool dev_boxes = false;
	};

	struct GameSession
	{
		dragon::assets::AssetManager assets;
		dragon::ui::AuthenticUiSystem ui;
		std::shared_ptr<codol::zone::Zone> map_zone;
		dragon::world::WorldScene world;
		dragon::world::CollisionWorld collision;
		dragon::world::EntityWorld entities;
		dragon::world::PlayerController player;
		dragon::world::WeaponSystem weapons;
		dragon::world::SkeletalRegistry skeletons;
		dragon::world::OfflineServer server;
		bool map_loaded = false;
		bool lobby_scene_loaded = false;
		dragon::ui::LobbyCameraSpec lobby_cam{};
		std::string active_map_name;
		std::string last_error;

		bool load_lobby_viewer(dragon::gfx::IRenderBackend& renderer, int max_tex_dim)
		{
			lobby_scene_loaded = false;
			std::string err;
			auto vz = assets.load_zone("ui_viewer_mp", &err);
			if (!vz)
			{
				dragon::platform::log_warn("[UI][MISSING] 3D lobby scene 'ui_viewer_mp.ff' unavailable: %s", err.c_str());
				return false;
			}
			if (!world.load_from_zone(*vz, assets, max_tex_dim, &err))
			{
				dragon::platform::log_warn("[UI][UNFINISHED] Could not decode 3D lobby GfxWorld from ui_viewer_mp.ff: %s", err.c_str());
				return false;
			}
			lobby_cam = ui.resolve_lobby_camera(*vz, "character_loadout_center");
			renderer.upload_scene(world);
			lobby_scene_loaded = true;
			dragon::platform::log_info("[UI][OK] Loaded 3D lobby backdrop ui_viewer_mp.ff (%u verts, cam=%s @ %.1f,%.1f,%.1f)",
				world.raw_world_vertex_count, lobby_cam.camera_targetname.c_str(),
				lobby_cam.eye.x, lobby_cam.eye.y, lobby_cam.eye.z);
			return true;
		}

		bool load_map(const std::string& name, const std::string& gametype_override,
			dragon::gfx::IRenderBackend& renderer, int max_tex_dim,
			bool use_intermission_cam, float yaw_off, float pitch_off, float z_off)
		{
			map_loaded = false;
			lobby_scene_loaded = false;
			last_error.clear();
			dragon::platform::log_info("Loading CODOL 1.1.1.13 map '%s'...", name.c_str());

			std::string err;
			map_zone = assets.load_zone(name, &err);
			if (!map_zone)
			{
				last_error = "Zone load failed: " + err;
				dragon::platform::log_error("%s", last_error.c_str());
				return false;
			}

			// Co-op mission zones (e.g. coop_defense_mp_crash_tropical) are mission overlays without
			// their own GfxWorld; resolve their embedded base map zone ("mp_crash_tropical") automatically.
			std::shared_ptr<codol::zone::Zone> world_zone = map_zone;
			if (!world.load_from_zone(*world_zone, assets, max_tex_dim, &err))
			{
				const std::size_t mp_pos = name.find("_mp_");
				if (mp_pos != std::string::npos)
				{
					const std::string base_name = name.substr(mp_pos + 1);
					dragon::platform::log_info("Zone '%s' is a mission overlay — loading base world '%s'",
						name.c_str(), base_name.c_str());
					auto base_zone = assets.load_zone(base_name, &err);
					if (base_zone && world.load_from_zone(*base_zone, assets, max_tex_dim, &err))
					{
						world_zone = std::move(base_zone);
					}
					else
					{
						last_error = "World decode failed: " + err;
						dragon::platform::log_error("%s", last_error.c_str());
						return false;
					}
				}
				else
				{
					last_error = "World decode failed: " + err;
					dragon::platform::log_error("%s", last_error.c_str());
					return false;
				}
			}

			collision.load_from_zone(*world_zone);
			if (!entities.load_from_zone(*map_zone))
				entities.load_from_zone(*world_zone);
			weapons.load_from_zone(*map_zone);
			if (weapons.inventory().empty() && world_zone != map_zone)
				weapons.load_from_zone(*world_zone);
			skeletons.index_zone(*world_zone);

			std::string gt = gametype_override;
			if (gt.empty())
			{
				if (name.rfind("zm_", 0) == 0) gt = "zm";
				else if (name.rfind("coop_", 0) == 0) gt = "coop";
				else gt = "war";
			}
			server.start_match(name, gt, entities, *map_zone, assets);

			dragon::world::SpawnPoint sp = entities.choose_initial_spawn(world.world_bounds);
			if (use_intermission_cam && !entities.intermissions().empty())
			{
				sp = entities.intermissions().front();
				player.spawn(sp.origin + dragon::math::Vec3{ 0, 0, z_off - 60.0f }, sp.yaw_deg + yaw_off, sp.pitch_deg + pitch_off, nullptr);
				player.state().noclip = true;
			}
			else
			{
				player.spawn(sp.origin + dragon::math::Vec3{ 0, 0, z_off }, sp.yaw_deg + yaw_off, sp.pitch_deg + pitch_off, &collision);
			}

			renderer.upload_scene(world);
			active_map_name = name;
			map_loaded = true;
			dragon::platform::log_info("Map '%s' ready! Spawn (%s): %.1f, %.1f, %.1f yaw=%.1f",
				name.c_str(), sp.classname.c_str(),
				player.state().origin.x, player.state().origin.y, player.state().origin.z,
				player.state().yaw_deg);
			dragon::platform::log_warn("[UI][UNFINISHED] In-game Scaleform HUD (scaleform/default/codohud.gfx) and character XModel rendering are NOT_IMPLEMENTED; synthetic dev overlays stay disabled unless explicitly enabled (--dev-hud / --dev-boxes).");
			return true;
		}
	};

	AppOptions parse_args(int argc, char** argv)
	{
		AppOptions opt;
#if defined(DRAGON_PS4) || defined(__PS4__) || defined(__ORBIS__)
		opt.width = 1920;
		opt.height = 1080;
		opt.max_frames = 0;
#else
		opt.width = 1280;
		opt.height = 720;
		opt.max_frames = 5;
#endif
		for (int i = 1; i < argc; ++i)
		{
			const std::string a = argv[i];
			auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
			if (a == "--game") opt.game_dir = next();
			else if (a == "--map") opt.initial_map = next();
			else if (a == "--gametype") opt.gametype = next();
			else if (a == "--render-out") opt.render_out_bmp = next();
			else if (a == "--stage") opt.force_stage = next();
			else if (a == "--load-progress") opt.load_progress = static_cast<float>(std::atof(next().c_str()));
			else if (a == "--frames") opt.max_frames = std::atoi(next().c_str());
			else if (a == "--width") opt.width = std::atoi(next().c_str());
			else if (a == "--height") opt.height = std::atoi(next().c_str());
			else if (a == "--max-tex") opt.max_tex_dim = std::atoi(next().c_str());
			else if (a == "--cam-yaw") opt.cam_yaw_offset = static_cast<float>(std::atof(next().c_str()));
			else if (a == "--cam-pitch") opt.cam_pitch_offset = static_cast<float>(std::atof(next().c_str()));
			else if (a == "--cam-z") opt.cam_z_offset = static_cast<float>(std::atof(next().c_str()));
			else if (a == "--intermission") opt.use_intermission_cam = true;
			else if (a == "--lobby-3d") opt.use_lobby_3d = true;
			else if (a == "--dev-map-menu") opt.dev_map_menu = true;
			else if (a == "--dev-hud") opt.dev_hud = true;
			else if (a == "--dev-crosshair") opt.dev_crosshair = true;
			else if (a == "--dev-boxes") opt.dev_boxes = true;
		}
		return opt;
	}

	dragon::ui::StartupStage parse_stage_name(const std::string& s)
	{
		if (s == "splash") return dragon::ui::StartupStage::Splash;
		if (s == "load") return dragon::ui::StartupStage::LoadingMenu;
		if (s == "login") return dragon::ui::StartupStage::LoginTitle;
		if (s == "logo") return dragon::ui::StartupStage::MainMenuLogo;
		if (s == "cyborgs_logo") return dragon::ui::StartupStage::CyborgsLogo;
		if (s == "mapload") return dragon::ui::StartupStage::MapLoading;
		if (s == "ingame") return dragon::ui::StartupStage::InGame;
		return dragon::ui::StartupStage::Splash;
	}
}

int main(int argc, char** argv)
{
	const AppOptions opt = parse_args(argc, argv);

	if (!dragon::platform::init(opt.game_dir.empty() ? nullptr : opt.game_dir.c_str()))
		return 1;

	auto renderer = dragon::gfx::create_platform_renderer();
	if (!renderer || !renderer->init(opt.width, opt.height))
	{
		dragon::platform::log_error("Failed to initialize graphics backend");
		dragon::platform::shutdown();
		return 2;
	}

	GameSession session;
	const std::string game_root = dragon::platform::info().game_data_dir;
	dragon::assets::MountReport mount_rep;
	if (!game_root.empty())
		mount_rep = session.assets.mount(game_root);

	std::vector<std::string> map_names;
	for (const auto& m : mount_rep.maps)
		map_names.push_back(m.name);

	// Initialize authentic UI assets (splash.bmp, Bink2 inspection, IW fonts/menus, Scaleform GFx)
	if (mount_rep.ok)
	{
		session.ui.init(session.assets);
	}
	else
	{
		dragon::platform::log_warn("CODOL 1.1.1.13 game files not mounted: %s",
			mount_rep.error.empty() ? "no game directory found" : mount_rep.error.c_str());
	}

	// Determine startup stage:
	// - Normal startup (no --map, no --stage): follows original client Splash -> IntroCinematics -> LoadingMenu -> LoginTitle
	// - Explicit --map: loads that map directly (used by map tests / dev workflows)
	// - Explicit --stage: jumps to the requested authentic startup/menu stage
	dragon::ui::StartupStage stage = dragon::ui::StartupStage::Splash;
	int current_map_idx = 0;

	if (!opt.force_stage.empty())
	{
		stage = parse_stage_name(opt.force_stage);
		if (stage == dragon::ui::StartupStage::InGame && mount_rep.ok && !opt.initial_map.empty())
		{
			session.load_map(opt.initial_map, opt.gametype, *renderer, opt.max_tex_dim,
				opt.use_intermission_cam, opt.cam_yaw_offset, opt.cam_pitch_offset, opt.cam_z_offset);
		}
		else if ((stage == dragon::ui::StartupStage::LoginTitle || stage == dragon::ui::StartupStage::MainMenuLogo) &&
		         opt.use_lobby_3d && mount_rep.ok)
		{
			session.load_lobby_viewer(*renderer, opt.max_tex_dim);
		}
	}
	else if (!opt.initial_map.empty() && mount_rep.ok)
	{
		for (std::size_t i = 0; i < map_names.size(); ++i)
			if (map_names[i] == opt.initial_map) { current_map_idx = static_cast<int>(i); break; }
		if (session.load_map(opt.initial_map, opt.gametype, *renderer, opt.max_tex_dim,
			opt.use_intermission_cam, opt.cam_yaw_offset, opt.cam_pitch_offset, opt.cam_z_offset))
		{
			stage = dragon::ui::StartupStage::InGame;
		}
	}

	// Register explicit developer console commands
	auto& console = dragon::platform::DebugConsole::instance();
	console.set_dvar("cg_drawDevHud", opt.dev_hud ? "1" : "0");
	console.set_dvar("cg_drawDevCrosshair", opt.dev_crosshair ? "1" : "0");
	console.set_dvar("cg_drawDevBoxes", opt.dev_boxes ? "1" : "0");

	bool show_dev_map_menu = opt.dev_map_menu;
	int menu_sel = current_map_idx;

	console.register_command("devmap", [&](const std::vector<std::string>& args) -> std::string {
		if (args.size() < 2) return "usage: devmap <mapname> [gametype]";
		const std::string gt = args.size() >= 3 ? args[2] : "";
		if (session.load_map(args[1], gt, *renderer, opt.max_tex_dim, false, 0, 0, 0))
		{
			stage = dragon::ui::StartupStage::InGame;
			return "Loaded " + args[1];
		}
		return "Failed: " + session.last_error;
	}, "[DEV TOOL] Load an original CODOL 1.1.1.13 map FastFile directly");

	console.register_command("devmapmenu", [&](const std::vector<std::string>&) -> std::string {
		show_dev_map_menu = !show_dev_map_menu;
		return show_dev_map_menu ? "Dev map selector opened" : "Dev map selector closed";
	}, "[DEV TOOL] Toggle the developer map selector overlay");

	console.register_command("maps", [&](const std::vector<std::string>&) -> std::string {
		return "Installed maps: " + std::to_string(map_names.size());
	}, "Show installed CODOL map count");

	dragon::platform::InputState input{};
	dragon::platform::PerfMetrics metrics{};
	std::vector<dragon::gfx::UiDrawCmd> ui_cmds;
	float options_hold_sec = 0.0f;
	float stage_elapsed_sec = 0.0f;
	bool intro_cinematics_logged = false;

	std::uint64_t last_ns = dragon::platform::time_now_ns();
	double fps_smooth = 60.0;

	for (int frame = 0; !dragon::platform::should_quit(); ++frame)
	{
		if (opt.max_frames > 0 && frame >= opt.max_frames)
			break;

		const std::uint64_t now_ns = dragon::platform::time_now_ns();
		float dt = static_cast<float>(now_ns - last_ns) * 1e-9f;
		last_ns = now_ns;
		if (dt <= 0.0f || dt > 0.1f) dt = 1.0f / 60.0f;
		stage_elapsed_sec += dt;

		dragon::platform::input_poll(input);

		// Hold Options for 3 seconds to exit cleanly on PS4
		if (input.down(dragon::platform::BTN_OPTIONS))
		{
			options_hold_sec += dt;
			if (options_hold_sec >= 3.0f)
			{
				dragon::platform::request_quit();
				break;
			}
		}
		else
		{
			options_hold_sec = 0.0f;
		}

		// Explicit developer tool triggers only (never triggered by normal OPTIONS press):
		//   L1 + R1 + TOUCHPAD => toggle [DEV TOOL] Map Selector
		//   TOUCHPAD alone     => toggle [DEV TOOL] Debug Console
		if (input.pressed(dragon::platform::BTN_TOUCHPAD))
		{
			if (input.down(dragon::platform::BTN_L1) && input.down(dragon::platform::BTN_R1))
				show_dev_map_menu = !show_dev_map_menu;
			else
				console.toggle_visible();
		}

		// Authentic startup progression when not pinned by --stage
		if (opt.force_stage.empty() && stage != dragon::ui::StartupStage::InGame)
		{
			if (stage == dragon::ui::StartupStage::Splash)
			{
				if (stage_elapsed_sec >= 2.5f || input.pressed(dragon::platform::BTN_CROSS))
				{
					stage = dragon::ui::StartupStage::IntroCinematics;
					stage_elapsed_sec = 0.0f;
				}
			}
			else if (stage == dragon::ui::StartupStage::IntroCinematics)
			{
				if (!intro_cinematics_logged)
				{
					intro_cinematics_logged = true;
					dragon::platform::log_warn("[UI][UNFINISHED] Skipping PUB_logo.bk2 & CODO_logo.bk2 (Bink 2 KB2i decoder NOT_IMPLEMENTED) -> advancing to ui_mp/load.txt");
				}
				stage = dragon::ui::StartupStage::LoadingMenu;
				stage_elapsed_sec = 0.0f;
			}
			else if (stage == dragon::ui::StartupStage::LoadingMenu)
			{
				if (stage_elapsed_sec >= 1.5f || input.pressed(dragon::platform::BTN_CROSS))
				{
					if (mount_rep.ok && !session.lobby_scene_loaded)
						session.load_lobby_viewer(*renderer, opt.max_tex_dim);
					stage = dragon::ui::StartupStage::LoginTitle;
					stage_elapsed_sec = 0.0f;
				}
			}
		}

		// Handle explicit [DEV TOOL] map selector when opened
		if (show_dev_map_menu && !map_names.empty())
		{
			if (input.pressed(dragon::platform::BTN_DPAD_UP))
				menu_sel = (menu_sel - 1 + static_cast<int>(map_names.size())) % static_cast<int>(map_names.size());
			if (input.pressed(dragon::platform::BTN_DPAD_DOWN))
				menu_sel = (menu_sel + 1) % static_cast<int>(map_names.size());
			if (input.pressed(dragon::platform::BTN_CIRCLE))
				show_dev_map_menu = false;
			if (input.pressed(dragon::platform::BTN_CROSS))
			{
				show_dev_map_menu = false;
				current_map_idx = menu_sel;
				if (session.load_map(map_names[static_cast<std::size_t>(current_map_idx)], "", *renderer, opt.max_tex_dim, false, 0, 0, 0))
					stage = dragon::ui::StartupStage::InGame;
			}
		}
		else if (stage == dragon::ui::StartupStage::InGame && session.map_loaded)
		{
			if (input.pressed(dragon::platform::BTN_DPAD_RIGHT))
				console.set_dvar("r_wireframe", console.dvar_bool("r_wireframe") ? "0" : "1");
			if (input.pressed(dragon::platform::BTN_DPAD_DOWN))
				console.set_dvar("r_lightmap", console.dvar_bool("r_lightmap", true) ? "0" : "1");

			session.player.update(input, dt, &session.collision);
			dragon::math::Vec3 fwd, right, up;
			dragon::math::angles_to_basis(session.player.state().yaw_deg,
				session.player.state().pitch_deg + session.weapons.recoil_pitch(), fwd, right, up);
			session.weapons.update(input, dt, session.player.state().eye_pos(), fwd,
				&session.collision, &session.server.bots());
			session.server.tick(dt, session.player.state().origin, &session.collision);
		}

		// Build authentic 2D UI draw commands for the active startup/menu stage
		const float load_prog = opt.force_stage.empty()
			? std::clamp(stage_elapsed_sec / 1.5f, 0.05f, 1.0f)
			: std::clamp(opt.load_progress, 0.0f, 1.0f);
		const std::string load_map_name = !opt.initial_map.empty() ? opt.initial_map : session.active_map_name;
		session.ui.build_stage_cmds(stage, renderer->draw_width(), renderer->draw_height(), load_prog, session.assets, ui_cmds, load_map_name);

		// Render frame
		dragon::gfx::RenderFrameParams rp;
		const bool draw_world = (stage == dragon::ui::StartupStage::InGame && session.map_loaded) ||
		                        ((stage == dragon::ui::StartupStage::LoginTitle || stage == dragon::ui::StartupStage::MainMenuLogo) && session.lobby_scene_loaded);
		rp.render_3d_scene = draw_world;
		rp.ui_cmds = &ui_cmds;

		if (stage == dragon::ui::StartupStage::InGame && session.map_loaded)
		{
			rp.eye = session.player.state().eye_pos();
			rp.yaw_deg = session.player.state().yaw_deg;
			rp.pitch_deg = std::clamp(session.player.state().pitch_deg + session.weapons.recoil_pitch(), -85.0f, 85.0f);
			const float base_fov = console.dvar_float("cg_fov", 75.0f);
			const float ads_fov = session.weapons.active_weapon().ads_fov;
			rp.fov_deg = base_fov + (ads_fov - base_fov) * session.player.state().ads_fraction;
		}
		else if (session.lobby_scene_loaded && session.lobby_cam.valid)
		{
			rp.eye = session.lobby_cam.eye;
			rp.yaw_deg = session.lobby_cam.yaw_deg;
			rp.pitch_deg = session.lobby_cam.pitch_deg;
			rp.fov_deg = session.lobby_cam.fov_deg;
		}

		rp.wireframe = console.dvar_bool("r_wireframe", false);
		rp.fullbright = console.dvar_bool("r_fullbright", false);
		rp.use_lightmap = console.dvar_bool("r_lightmap", true);
		rp.draw_static_models = console.dvar_bool("r_drawStaticModels", true);
		rp.frustum_cull = console.dvar_bool("r_frustumCull", true);

		// Synthetic overlays strictly gated behind explicit developer flags (all OFF by default)
		rp.show_dev_hud = console.dvar_bool("cg_drawDevHud", false);
		rp.show_dev_crosshair = console.dvar_bool("cg_drawDevCrosshair", false);
		rp.show_dev_boxes = console.dvar_bool("cg_drawDevBoxes", false);
		rp.show_console = console.visible();
		rp.show_dev_map_menu = show_dev_map_menu;

		rp.map_name = session.active_map_name;
		rp.gametype = session.server.gametype();
		rp.weapon_name = session.weapons.active_weapon().display_name;
		rp.ammo_clip = session.weapons.active_weapon().ammo_in_clip;
		rp.ammo_reserve = session.weapons.active_weapon().ammo_reserve;
		rp.reloading = session.weapons.is_reloading();
		rp.health = session.player.state().health;
		rp.score = session.server.player_score();
		rp.kills = session.server.player_kills();
		rp.console_lines = console.recent_lines(28);
		rp.menu_items = map_names;
		rp.menu_selected = menu_sel;
		rp.bots = &session.server.bots();
		rp.impacts = &session.weapons.recent_impacts();

		metrics.frame_index = static_cast<std::uint64_t>(frame);
		metrics.zone_memory_bytes = dragon::platform::MemoryStats::instance().bytes(dragon::platform::MemTag::Zones);
		metrics.texture_memory_bytes = dragon::platform::MemoryStats::instance().bytes(dragon::platform::MemTag::Textures);
		metrics.geometry_memory_bytes = dragon::platform::MemoryStats::instance().bytes(dragon::platform::MemTag::Geometry);
		metrics.gpu_memory_bytes = dragon::platform::MemoryStats::instance().bytes(dragon::platform::MemTag::Gpu);

		const dragon::world::WorldScene* active_scene = draw_world ? &session.world : nullptr;
		renderer->render_frame(active_scene, rp, metrics);
		const double inst_fps = metrics.frame_ms > 0.01 ? (1000.0 / metrics.frame_ms) : 60.0;
		fps_smooth = fps_smooth * 0.85 + inst_fps * 0.15;
		metrics.fps = fps_smooth;

		if (!opt.render_out_bmp.empty() && frame == 0)
		{
			if (renderer->save_screenshot_bmp(opt.render_out_bmp))
			{
				dragon::platform::log_info("Saved rendered CODOL frame -> %s (%ux%u, ui_cmds=%zu, surfs=%u, tris=%u, %.1f ms)",
					opt.render_out_bmp.c_str(), opt.width, opt.height, ui_cmds.size(),
					metrics.visible_surfaces, metrics.drawn_triangles, metrics.frame_ms);
			}
		}
	}

	dragon::platform::log_info("Session summary: map='%s', backend='%s', drawn_tris=%u, visible_surfs=%u, frame_ms=%.1f",
		session.active_map_name.c_str(), renderer->backend_name(),
		metrics.drawn_triangles, metrics.visible_surfaces, metrics.frame_ms);

	renderer->shutdown();
	dragon::platform::shutdown();
	return 0;
}
