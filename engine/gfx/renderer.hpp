#pragma once

// Project Dragon — Phase 2 & 4 Native Graphics Renderer Abstraction
// Backends:
//   - PS4 Native Hardware GPU: Piglet OpenGL ES 2.0 (dynamic libScePigletv2VSH + libSceShaccVSH)
//   - PS4 Native Hardware Vulkan: ps4-kit Mesa/RADV on GNM (when built with -DDRAGON_PS4_VULKAN=ON)
//   - PS4 Native VideoOut Direct-Memory Hardware Scanout + Multithreaded 3D Textured Rasterizer (zero-.sprx fallback)
//   - PC Debug Renderer: Multithreaded 3D Textured Rasterizer + BMP/PPM frame capture

#include "../platform/platform.hpp"
#include "../world/bsp_world.hpp"
#include "../world/gameplay.hpp"

#include <memory>
#include <string>
#include <vector>

namespace dragon::gfx
{
	enum class UiBlendMode : std::uint8_t
	{
		Alpha = 0,    // Standard straight-alpha src-over
		Additive = 1, // SWF BlendMode 8 (Add)
		Opaque = 2    // Copy RGB directly
	};

	struct UiDrawCmd
	{
		float x = 0.0f;
		float y = 0.0f;
		float w = 0.0f;
		float h = 0.0f;
		float u0 = 0.0f;
		float v0 = 0.0f;
		float u1 = 1.0f;
		float v1 = 1.0f;
		float r = 1.0f;
		float g = 1.0f;
		float b = 1.0f;
		float a = 1.0f;
		UiBlendMode blend = UiBlendMode::Alpha;
		const assets::DecodedTexture* texture = nullptr; // nullptr => solid rect
	};

	struct RenderFrameParams
	{
		math::Vec3 eye{ 0, 0, 64 };
		float yaw_deg = 0.0f;
		float pitch_deg = 0.0f;
		float fov_deg = 75.0f;

		bool render_3d_scene = true;
		bool wireframe = false;
		bool fullbright = false;
		bool use_lightmap = true;
		bool draw_static_models = true;
		bool frustum_cull = true;

		// Explicit developer overlays — all OFF by default so normal startup/gameplay shows only authentic client graphics.
		bool show_dev_hud = false;
		bool show_dev_crosshair = false;
		bool show_dev_boxes = false;
		bool show_console = false;
		bool show_dev_map_menu = false;

		// Authentic 2D UI draw list sourced from original client assets (splash.bmp, IW menuDef_t, Scaleform GFx, Font_s)
		const std::vector<UiDrawCmd>* ui_cmds = nullptr;

		std::string map_name;
		std::string gametype;
		std::string weapon_name;
		int ammo_clip = 0;
		int ammo_reserve = 0;
		bool reloading = false;
		int health = 100;
		int score = 0;
		int kills = 0;

		std::vector<std::string> status_lines;
		std::vector<std::string> console_lines;
		std::vector<std::string> menu_items;
		int menu_selected = 0;

		const std::vector<world::BotEntity>* bots = nullptr;
		const std::vector<world::WeaponImpact>* impacts = nullptr;
	};

	class IRenderBackend
	{
	public:
		virtual ~IRenderBackend() = default;
		virtual bool init(int width, int height) = 0;
		virtual void shutdown() = 0;
		virtual bool upload_scene(const world::WorldScene& scene) = 0;
		virtual void render_frame(const world::WorldScene* scene, const RenderFrameParams& params, platform::PerfMetrics& metrics) = 0;
		virtual const char* backend_name() const = 0;
		virtual bool save_screenshot_bmp(const std::string& path) const = 0;
	};

	std::unique_ptr<IRenderBackend> create_platform_renderer();
}
