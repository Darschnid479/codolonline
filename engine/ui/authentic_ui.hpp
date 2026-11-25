#pragma once

// Project Dragon — Authentic Call of Duty Online 1.1.1.13 UI, Startup, Font & Menu Loader
// Strictly data-driven from user-supplied original client files:
//   - Root BMPs: splash.bmp (Sys_ShowSplashWindow @ 0x00632908), logo.bmp (Sys_CreateConsole @ 0x00632f25)
//   - Bink 2 intro cinematics: main/video/PUB_logo.bk2 & CODO_logo.bk2 (header-inspected; KB2i decoder NOT_IMPLEMENTED)
//   - IW FastFile fonts (ASSET_TYPE_FONT) & menus (ASSET_TYPE_MENU: ui_mp/load.txt, ui_mp/code.txt, ui_mp/error.txt)
//   - Scaleform GFx movies (scaleform/default/codologin.gfx, scaleform/default/codo.gfx) display-list external bitmaps
//   - Localized strings (ASSET_TYPE_LOCALIZE_ENTRY from localized_code_post_gfx_mp.ff)
// Zero invented menus, zero AI-generated logos, zero synthetic fallbacks. Missing or unsupported formats are logged explicitly.

#include "../assets/asset_manager.hpp"
#include "../gfx/renderer.hpp"

#include <map>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dragon::ui
{
	enum class StartupStage : std::uint8_t
	{
		Splash = 0,         // Original root splash.bmp (800x510)
		IntroCinematics,    // Original PUB_logo.bk2 & CODO_logo.bk2 (logged as KB2i NOT_IMPLEMENTED)
		LoadingMenu,        // Original patch_code_post_gfx_mp.ff ui_mp/load.txt -> MENU load (loadscreen_menu + loadbar)
		LoginTitle,         // Original scaleform/default/codologin.gfx -> loginView (with authentic startup logo)
		MainMenuLogo,       // Original scaleform/default/codo.gfx -> logoView (main menu logo)
		CyborgsLogo,        // Original scaleform/default/codo.gfx -> logoCyborgsView (Zombies/Cyborgs logo)
		MapLoading,         // Original patch_code_post_gfx_mp.ff MENU connect (loadscreen_<map> + loadbar)
		InGame              // Active 3D world gameplay
	};

	struct Bink2Info
	{
		bool found = false;
		bool valid_header = false;
		std::string rel_path;
		char magic[5]{};
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t frame_count = 0;
		std::uint32_t fps_num = 0;
		std::uint32_t fps_den = 1;
	};

	struct IwGlyph
	{
		std::uint16_t letter = 0;
		std::int8_t x0 = 0;
		std::int8_t y0 = 0;
		std::uint8_t dx = 0;
		std::uint8_t pixel_width = 0;
		std::uint8_t pixel_height = 0;
		float s0 = 0.0f, t0 = 0.0f, s1 = 0.0f, t1 = 0.0f;
	};

	struct IwFont
	{
		std::string name;
		std::string source_zone;
		std::string material_name;
		int pixel_height = 0;
		std::shared_ptr<assets::DecodedTexture> atlas;
		std::unordered_map<std::uint16_t, IwGlyph> glyphs;

		bool is_valid() const { return pixel_height > 0 && atlas && atlas->is_valid() && !glyphs.empty(); }
		void emit_text(const std::string& text, float x, float y, float scale,
			float r, float g, float b, float a, std::vector<gfx::UiDrawCmd>& out,
			int* missing_glyphs = nullptr) const;
	};

	struct IwMenuItem
	{
		std::string name;
		float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
		std::uint8_t horiz_align = 0;
		std::uint8_t vert_align = 0;
		std::int32_t style = 0;
		std::int32_t owner_draw = 0;
		float fore_color[4]{ 1, 1, 1, 1 };
		float back_color[4]{ 0, 0, 0, 0 };
		std::string background_material;
		std::string text;
		std::shared_ptr<assets::DecodedTexture> resolved_texture;
		std::string texture_origin;
	};

	struct IwMenuDef
	{
		std::string name;
		std::string source_zone;
		float x = 0.0f, y = 0.0f, w = 640.0f, h = 480.0f;
		std::vector<IwMenuItem> items;
		bool is_valid() const { return !name.empty() && !items.empty(); }
	};

	struct ScaleformExternalImage
	{
		std::uint32_t char_id = 0;
		std::uint16_t width = 0;
		std::uint16_t height = 0;
		std::string export_name;
		std::string file_name;
		std::shared_ptr<assets::DecodedTexture> texture;
		std::string source_origin; // e.g. "ui_mp.ff -> iw_image_03.iwd:images/ui_a334dfc81d87a513.iwi"
	};

	struct ScaleformPlacement
	{
		std::uint16_t depth = 0;
		bool has_char = false;
		std::uint16_t char_id = 0;
		bool has_matrix = false;
		float tx = 0.0f, ty = 0.0f;
		float sx = 1.0f, sy = 1.0f;
		bool has_cxform = false;
		float mul_r = 1.0f, mul_g = 1.0f, mul_b = 1.0f, mul_a = 1.0f;
		std::uint8_t blend_mode = 0; // 8 = Additive
		bool is_clip_mask = false;
		std::string instance_name;
		std::string class_name;
	};

	struct ScaleformSprite
	{
		std::uint16_t id = 0;
		std::uint16_t frame_count = 0;
		std::vector<ScaleformPlacement> frame0_places;
	};

	struct ScaleformGfxMovie
	{
		std::string rawfile_name;
		std::string source_zone;
		float stage_x = 0.0f, stage_y = 0.0f;
		float stage_width = 1280.0f;
		float stage_height = 960.0f;
		std::uint8_t swf_version = 0;
		std::uint32_t do_abc_tag_count = 0;
		std::uint32_t define_shape_tag_count = 0;
		std::uint32_t define_edit_text_tag_count = 0;
		std::unordered_map<std::uint32_t, ScaleformExternalImage> external_images;
		std::unordered_map<std::uint16_t, ScaleformSprite> sprites;
		std::unordered_map<std::string, std::uint16_t> exports_by_name;

		// Explicit first-frame instances on the movie timeline, keyed by depth.
		std::map<std::uint16_t, ScaleformPlacement> root_instances;
		std::unordered_map<std::string, std::uint16_t> symbol_classes;

		bool loaded() const { return !rawfile_name.empty() && stage_width > 0.0f && stage_height > 0.0f; }

		// Emit decoded external bitmaps referenced by an exported sprite (e.g. "loginView", "logoView", "logoCyborgsView")
		// at their authentic transformed coordinates mapped from stage_width x stage_height onto fb_w x fb_h.
		void emit_export(const std::string& export_name, int fb_w, int fb_h,
			assets::AssetManager& assets, std::vector<gfx::UiDrawCmd>& out,
			bool center_sprite_on_screen = false,
			std::vector<std::string>* resolved_origins = nullptr,
			std::vector<std::string>* unfinished_notes = nullptr);
	};

	struct LobbyCameraSpec
	{
		bool valid = false;
		std::string preset_name;
		std::string camera_targetname;
		std::string look_targetname;
		math::Vec3 eye{};
		float yaw_deg = 0.0f;
		float pitch_deg = 0.0f;
		float fov_deg = 50.0f;
	};

	// Utility loaders
	std::shared_ptr<assets::DecodedTexture> load_bmp_texture(const std::string& path, const std::string& name, std::string* error = nullptr);
	Bink2Info inspect_bink2_file(const std::string& full_path, const std::string& rel_path);

	class AuthenticUiSystem
	{
	public:
		bool init(assets::AssetManager& assets);

		// Emit authentic 2D draw commands for the requested startup/menu stage (no invented UI)
		void build_stage_cmds(StartupStage stage, int fb_w, int fb_h, float load_progress,
			assets::AssetManager& assets, std::vector<gfx::UiDrawCmd>& out,
			const std::string& active_map = "");

		// Read authentic 3D lobby camera from patch_code_post_gfx_mp.ff (mp/camerapostable.csv) + ui_viewer_mp.ff (MapEnts)
		LobbyCameraSpec resolve_lobby_camera(const codol::zone::Zone& ui_viewer_zone, const std::string& preset_name = "character_loadout_center") const;

		// Accessors for verification and unit tests
		const std::shared_ptr<assets::DecodedTexture>& splash_bmp() const { return splash_bmp_; }
		const std::shared_ptr<assets::DecodedTexture>& console_logo_bmp() const { return logo_bmp_; }
		const Bink2Info& pub_logo_bk2() const { return pub_logo_bk2_; }
		const Bink2Info& codo_logo_bk2() const { return codo_logo_bk2_; }
		const IwMenuDef* find_menu(const std::string& name) const;
		const IwFont* find_font(const std::string& name) const;
		const ScaleformGfxMovie& codologin_gfx() const { return codologin_gfx_; }
		const ScaleformGfxMovie& codo_gfx() const { return codo_gfx_; }
		const std::vector<std::string>& unfinished_log() const { return unfinished_log_; }
		std::string localize(const std::string& key) const;

	private:
		bool load_iw_fonts_and_menus(assets::AssetManager& assets, const std::string& zone_name);
		bool load_scaleform_movie(assets::AssetManager& assets, const std::string& rawfile_name,
			const std::vector<std::string>& candidate_zones, ScaleformGfxMovie& out);
		bool parse_scaleform_bytes(const std::vector<std::uint8_t>& bytes, ScaleformGfxMovie& out);
		void emit_iw_menu(const IwMenuDef& menu, int fb_w, int fb_h, float load_progress,
			assets::AssetManager& assets, const std::string& active_map, std::vector<gfx::UiDrawCmd>& out);

		std::shared_ptr<assets::DecodedTexture> splash_bmp_;
		std::shared_ptr<assets::DecodedTexture> logo_bmp_;
		Bink2Info pub_logo_bk2_{};
		Bink2Info codo_logo_bk2_{};

		std::unordered_map<std::string, IwFont> fonts_;
		std::unordered_map<std::string, IwMenuDef> menus_;
		std::unordered_map<std::string, std::string> localized_;
		ScaleformGfxMovie codologin_gfx_;
		ScaleformGfxMovie codo_gfx_;
		std::shared_ptr<codol::zone::Zone> patch_code_post_gfx_zone_;

		std::vector<std::string> unfinished_log_;
		bool logged_login_unfinished_ = false;
		bool logged_logo_unfinished_ = false;
	};
}
