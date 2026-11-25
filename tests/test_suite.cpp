// Project Dragon — Automated Unit & Real-Asset Integration Test Suite

#include "../engine/assets/asset_manager.hpp"
#include "../engine/gfx/software_rasterizer.hpp"
#include "../engine/platform/platform.hpp"
#include "../engine/ui/authentic_ui.hpp"
#include "../engine/world/bsp_world.hpp"
#include "../engine/world/collision.hpp"
#include "../engine/world/entities.hpp"
#include "../engine/world/gameplay.hpp"
#include "../engine/world/player_movement.hpp"
#include "../third_party/codol_zone_portable/codol_zone/src/crypto_win.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_passed = 0;
	int g_failed = 0;

	void check(bool cond, const char* name)
	{
		if (cond)
		{
			++g_passed;
			std::cout << "  [PASS] " << name << "\n";
		}
		else
		{
			++g_failed;
			std::cerr << "  [FAIL] " << name << "\n";
		}
	}

	void test_login_placement()
	{
		dragon::ui::ScaleformGfxMovie movie;
		movie.stage_x = -10;
		movie.stage_y = -20;
		movie.exports_by_name["loginView"] = 1;
		movie.symbol_classes["buttonClass"] = 2;
		dragon::ui::ScaleformPlacement root;
		root.has_char = true;
		root.char_id = 1;
		root.tx = 30;
		root.ty = 40;
		root.sx = root.sy = 2;
		movie.root_instances[7] = root;
		dragon::ui::ScaleformPlacement button;
		button.class_name = "buttonClass";
		button.tx = 4;
		button.ty = 5;
		movie.sprites[1].frame0_places.push_back(button);
		dragon::ui::ScaleformPlacement bitmap;
		bitmap.has_char = true;
		bitmap.char_id = 3;
		movie.sprites[2].frame0_places.push_back(bitmap);
		auto& image = movie.external_images[3];
		image.width = image.height = 1;
		image.texture = std::make_shared<dragon::assets::DecodedTexture>();
		image.texture->width = image.texture->height = 1;
		image.texture->rgba = {255, 255, 255, 255};
		dragon::assets::AssetManager assets;
		std::vector<dragon::gfx::UiDrawCmd> commands;
		movie.emit_export("loginView", 1280, 960, assets, commands);
		check(commands.size() == 1 && commands[0].x == 48 && commands[0].y == 70 &&
			commands[0].w == 2 && commands[0].texture == image.texture.get(),
			"Place a class-linked bitmap at the movie's root and frame origin");
	}

	void test_crypto_primitives()
	{
		std::cout << "--- Crypto (SHA-256, AES-192-CTR LE/BE, CRC-32) ---\n";
		// FIPS 180-4 SHA-256("")
		std::uint8_t digest[32]{};
		codol::zone::crypto::sha256({}, digest);
		static constexpr std::uint8_t kEmptySha256[32] = {
			0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
			0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
		};
		check(std::memcmp(digest, kEmptySha256, 32) == 0, "SHA-256 empty string matches FIPS 180-4");

		const char* abc = "abc";
		codol::zone::crypto::sha256({ reinterpret_cast<const std::uint8_t*>(abc), 3 }, digest);
		static constexpr std::uint8_t kAbcSha256[32] = {
			0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
			0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
		};
		check(std::memcmp(digest, kAbcSha256, 32) == 0, "SHA-256(\"abc\") matches FIPS 180-4");

		// FIPS 197 Appendix C.2 AES-192 single-block encryption test vector
		static constexpr std::uint8_t kAes192Key[24] = {
			0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,
			0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17
		};
		static constexpr std::uint8_t kAes192Pt[16] = {
			0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
		};
		static constexpr std::uint8_t kAes192Ct[16] = {
			0xdd,0xa9,0x7c,0xa4,0x86,0x4c,0xdf,0xe0,0x6e,0xaf,0x70,0xa0,0xec,0x0d,0x71,0x91
		};
		codol::zone::crypto::AesBlock blk;
		check(blk.init(kAes192Key), "AES-192 key schedule init");
		std::uint8_t ct[16]{};
		blk.encrypt_block(kAes192Pt, ct);
		check(std::memcmp(ct, kAes192Ct, 16) == 0, "AES-192 ECB block matches FIPS 197 C.2");

		// AES-192-CTR LE & BE symmetry
		codol::zone::crypto::AesCtr ctr;
		ctr.init(kAes192Key);
		const std::uint8_t iv[16] = { 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
		std::vector<std::uint8_t> buf(100);
		for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<std::uint8_t>(i * 7 + 3);
		const auto orig = buf;
		ctr.transform(iv, buf);
		check(buf != orig, "AES-192-CTR LE encrypts buffer");
		ctr.transform(iv, buf);
		check(buf == orig, "AES-192-CTR LE decrypts back identically");
		ctr.transform_be(iv, buf);
		check(buf != orig, "AES-192-CTR BE encrypts buffer");
		ctr.transform_be(iv, buf);
		check(buf == orig, "AES-192-CTR BE decrypts back identically");

		// Standard ZIP CRC-32("123456789") == 0xCBF43926
		const char* s19 = "123456789";
		const std::uint32_t crc = codol::render::crc32({ reinterpret_cast<const std::uint8_t*>(s19), 9 });
		check(crc == 0xCBF43926u, "CRC-32(\"123456789\") == 0xCBF43926");
	}

	void test_collision_and_movement()
	{
		std::cout << "--- Collision (ClipMap) & Player Movement (PM_Move) ---\n";
		dragon::world::CollisionWorld coll;
		// Solid floor z in [-32, 0] and wall x in [100, 132]
		coll.add_synthetic_box({ -512, -512, -32 }, { 512, 512, 0 });
		coll.add_synthetic_box({ 100, -128, 0 }, { 132, 128, 128 });

		// Raycast straight down hits z = 0
		const auto rdown = coll.ray_trace({ 0, 0, 64 }, { 0, 0, -64 });
		check(rdown.fraction < 1.0f && std::fabs(rdown.endpos.z) < 0.2f && rdown.normal.z > 0.9f, "CM_RayTrace hits floor cleanly");

		// Player spawns above floor, drops to floor, walks into wall and stops at x <= 85 (100 - 15 bbox)
		dragon::world::PlayerController pc;
		pc.spawn({ 0, 0, 48 }, 0.0f, 0.0f, &coll);
		check(pc.state().on_ground && std::fabs(pc.state().origin.z) < 2.0f, "PlayerController spawns onto floor via CM_BoxTrace");

		dragon::platform::InputState in{};
		in.ly = 1.0f; // move +X toward wall at x=100
		for (int i = 0; i < 60; ++i)
			pc.update(in, 1.0f / 60.0f, &coll);
		check(pc.state().origin.x > 60.0f && pc.state().origin.x <= 85.2f, "PM_Move stops cleanly against brush wall");
	}

	void test_entities_and_gsc()
	{
		std::cout << "--- MapEnts Parser & Clean-Room GSC VM ---\n";
		dragon::world::EntityWorld ew;
		const char* sample_ents = R"(
		{
			"classname" "worldspawn"
			"suncolor" "1 0.9 0.8"
		}
		{
			"classname" "mp_tdm_spawn"
			"origin" "128 256 16"
			"angles" "0 90 0"
		}
		{
			"classname" "node_pathnode"
			"origin" "300 256 16"
		}
		)";
		check(ew.parse_entity_string(sample_ents), "MapEnts parser decodes entity blocks");
		check(ew.player_spawns().size() == 1 && std::fabs(ew.player_spawns()[0].origin.x - 128.0f) < 0.1f, "MapEnts extracts mp_tdm_spawn origin");
		check(ew.pathnodes().size() == 1, "MapEnts extracts node_pathnode waypoint");

		dragon::world::GscRuntime gsc;
		const char* sample_gsc = R"(
		main()
		{
			setDvar("test_gsc_dvar", "42");
			level.game_ready = 1;
			thread watcher();
			wait 0.1;
			notify("go_signal");
		}
		watcher()
		{
			waittill("go_signal");
			level.watcher_fired = 1;
		}
		)";
		check(gsc.load_script_source("maps/mp/test.gsc", sample_gsc), "GSC parser loads script functions");
		gsc.spawn_thread("maps/mp/test.gsc", "main");
		for (int i = 0; i < 10; ++i) gsc.tick(0.05f);
		check(dragon::platform::DebugConsole::instance().dvar_int("test_gsc_dvar") == 42, "GSC setDvar executes");
		check(gsc.level_vars().count("watcher_fired") == 1, "GSC thread + wait + waittill/notify executes");
	}

	void test_real_codol_install()
	{
		const std::string game_dir = dragon::platform::locate_game_install(nullptr);
		if (game_dir.empty() || !dragon::platform::dir_exists(game_dir))
		{
			std::cout << "--- Real CODOL 1.1.1.13 Integration Tests (SKIPPED: no game dir found) ---\n";
			return;
		}
		std::cout << "--- Real CODOL 1.1.1.13 Integration Tests (" << game_dir << ") ---\n";
		dragon::assets::AssetManager mgr;
		const auto rep = mgr.mount(game_dir);
		check(rep.ok && rep.has_fastfile_key && rep.has_iwd_key && rep.has_wavelet_tables, "Mount real CODOL 1.1.1.13 keys & wavelet tables");
		check(rep.maps.size() >= 10, "Discovered real CODOL 1.1.1.13 map FastFiles");

		// Pick mp_killhouse_sh (or first map)
		std::string map_name = rep.maps.front().name;
		for (const auto& m : rep.maps) if (m.name == "mp_killhouse_sh") { map_name = m.name; break; }

		std::string err;
		auto zone = mgr.load_zone(map_name, &err);
		check(zone != nullptr, ("Decrypt + inflate + load real FastFile v334: " + map_name).c_str());
		if (zone)
		{
			dragon::world::WorldScene ws;
			check(ws.load_from_zone(*zone, mgr, 128, &err), "Decode real GfxWorld + materials + textures");
			check(ws.raw_world_vertex_count > 1000 && ws.resolved_textured_surfaces > 10, "Real map has valid geometry & decoded IWI textures");

			dragon::world::CollisionWorld cm;
			check(cm.load_from_zone(*zone) && cm.brush_count() > 50, "Decode real ClipMap collision brushes");

			dragon::world::EntityWorld ew;
			check(ew.load_from_zone(*zone) && !ew.entities().empty(), "Decode real MapEnts entities");

			// Render one real frame to verify end-to-end pipeline produces non-trivial image
			dragon::gfx::SoftwareRasterizer rast;
			rast.init(320, 180, 2);
			dragon::gfx::RenderFrameParams rp;
			const auto sp = ew.choose_initial_spawn(ws.world_bounds);
			rp.eye = sp.origin + dragon::math::Vec3{ 0, 0, 60 };
			rp.yaw_deg = sp.yaw_deg;
			rp.pitch_deg = sp.pitch_deg;
			rp.map_name = map_name;
			dragon::platform::PerfMetrics pm{};
			rast.render(&ws, rp, pm);
			check(pm.drawn_triangles > 100, "Rendered real CODOL map geometry (>100 visible triangles)");

			// Authentic UI & startup assets tests
			dragon::ui::AuthenticUiSystem ui;
			check(ui.init(mgr), "AuthenticUiSystem mounts original startup & UI assets");
			check(ui.splash_bmp() && ui.splash_bmp()->width == 800 && ui.splash_bmp()->height == 510,
				"Loaded original root splash.bmp (800x510)");
			check(ui.pub_logo_bk2().valid_header && ui.codo_logo_bk2().valid_header,
				"Inspected Bink 2 intro headers (PUB_logo.bk2 & CODO_logo.bk2)");
			check(ui.find_font("fonts/bigFont") && ui.find_font("fonts/bigFont")->is_valid(),
				"Decoded original IW Font_s (fonts/bigFont) from code_post_gfx_mp.ff");
			check(ui.find_menu("load") && ui.find_menu("load")->items.size() == 3,
				"Decoded original IW menuDef_t (ui_mp/load.txt -> load) from patch_code_post_gfx_mp.ff");

			std::vector<dragon::gfx::UiDrawCmd> cmds;
			ui.build_stage_cmds(dragon::ui::StartupStage::LoginTitle, 1280, 720, 1.0f, mgr, cmds);
			check(cmds.size() >= 3, "Scaleform codologin.gfx loginView emits authentic decoded bitmap layers");
			cmds.clear();
			ui.build_stage_cmds(dragon::ui::StartupStage::MainMenuLogo, 1280, 720, 1.0f, mgr, cmds);
			check(cmds.size() == 3, "Scaleform codo.gfx logoView emits 3 authentic logo bitmap layers");

			rast.shutdown();
		}
	}
}

int main()
{
	dragon::platform::init(nullptr);
	test_crypto_primitives();
	test_collision_and_movement();
	test_entities_and_gsc();
	test_login_placement();
	test_real_codol_install();
	dragon::platform::shutdown();

	std::cout << "\n=== TEST SUMMARY: " << g_passed << " passed, " << g_failed << " failed ===\n";
	return g_failed == 0 ? 0 : 1;
}
