#pragma once

// Project Dragon — Multithreaded Perspective-Correct Textured 3D Software Rasterizer
// Powers:
//   1. Native PS4 libSceVideoOut Direct-Memory scanout fallback (when external Piglet/RADV GPU libs aren't present)
//   2. PC Debug visual verification & automated map render capture (BMP/PPM)

#include "renderer.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dragon::gfx
{
	class SoftwareRasterizer
	{
	public:
		SoftwareRasterizer() = default;
		~SoftwareRasterizer() = default;

		bool init(int width, int height, int worker_threads = 0);
		void shutdown();

		void render(const world::WorldScene* scene, const RenderFrameParams& params, platform::PerfMetrics& metrics);

		int width() const { return width_; }
		int height() const { return height_; }
		// 0xAARRGGBB (little-endian BGRA bytes — matches PS4 VideoOut SRGB & Windows BMP directly!)
		const std::uint32_t* pixels_bgra() const { return color_buf_.data(); }

		// Blit into a target 32-bit BGRA framebuffer (e.g. PS4 1920x1080 VideoOut direct memory) with integer/nearest scale
		void blit_to_bgra(std::uint32_t* dst, int dst_w, int dst_h, int dst_pitch_pixels) const;

		bool save_bmp(const std::string& path) const;
		bool save_ppm(const std::string& path) const;

	private:
		struct ClipVertex
		{
			float x, y, z, w; // clip space
			float u, v;
			float lu, lv;
			float r, g, b;    // shaded vertex light
		};

		struct ScreenTri
		{
			float x[3], y[3], inv_w[3];
			float u_w[3], v_w[3];
			float lu_w[3], lv_w[3];
			float r_w[3], g_w[3], b_w[3];
			int min_x, max_x, min_y, max_y;
			const assets::DecodedTexture* diffuse = nullptr;
			const assets::DecodedTexture* lightmap = nullptr;
			bool alpha_test = false;
			bool wireframe = false;
		};

		void clear_sky_and_depth(float pitch_deg);
		void submit_triangle(const world::RenderVertex& v0, const world::RenderVertex& v1, const world::RenderVertex& v2,
			const math::Mat4& vp, const math::Vec3& sun_dir, bool fullbright, bool wireframe,
			const assets::DecodedTexture* diffuse, const assets::DecodedTexture* lightmap, bool alpha_test,
			std::vector<ScreenTri>& out_tris);
		void rasterize_band(int y0, int y1, const std::vector<ScreenTri>& tris);
		void draw_world_box(const math::Vec3& mins, const math::Vec3& maxs, const math::Mat4& vp, std::uint32_t bgra);
		void draw_line_2d(int x0, int y0, int x1, int y1, std::uint32_t bgra);
		void draw_rect_alpha(int x, int y, int w, int h, std::uint32_t bgra, std::uint8_t alpha);
		void draw_text(int x, int y, const std::string& text, std::uint32_t bgra, int scale = 1);
		void draw_hud(const world::WorldScene* scene, const RenderFrameParams& params, const platform::PerfMetrics& metrics);

		int width_ = 0;
		int height_ = 0;
		std::vector<std::uint32_t> color_buf_;
		std::vector<float> depth_buf_;
		std::unique_ptr<platform::ThreadPool> pool_;
		std::vector<ScreenTri> frame_tris_;
	};
}
