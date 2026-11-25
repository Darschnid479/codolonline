#include "software_rasterizer.hpp"
#include "font8x8.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dragon::gfx
{
	namespace
	{
		inline std::uint32_t pack_bgra(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255)
		{
			return (static_cast<std::uint32_t>(a) << 24) |
			       (static_cast<std::uint32_t>(r) << 16) |
			       (static_cast<std::uint32_t>(g) << 8)  |
			       static_cast<std::uint32_t>(b);
		}

		inline float edge_fn(float ax, float ay, float bx, float by, float px, float py)
		{
			return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
		}

		inline int wrap_texcoord(float tc, int dim)
		{
			float f = tc - std::floor(tc);
			int i = static_cast<int>(f * static_cast<float>(dim));
			if (i < 0) i = 0;
			if (i >= dim) i = dim - 1;
			return i;
		}
	}

	bool SoftwareRasterizer::init(int width, int height, int worker_threads)
	{
		width_ = std::max(64, width);
		height_ = std::max(64, height);
		color_buf_.assign(static_cast<std::size_t>(width_) * height_, pack_bgra(20, 28, 38));
		depth_buf_.assign(static_cast<std::size_t>(width_) * height_, 0.0f);
		pool_ = std::make_unique<platform::ThreadPool>(worker_threads);
		frame_tris_.reserve(65536);
		return true;
	}

	void SoftwareRasterizer::shutdown()
	{
		pool_.reset();
		color_buf_.clear();
		depth_buf_.clear();
	}

	void SoftwareRasterizer::clear_sky_and_depth(float pitch_deg)
	{
		const float horizon_shift = pitch_deg / 90.0f;
		for (int y = 0; y < height_; ++y)
		{
			const float fy = (static_cast<float>(y) / static_cast<float>(height_)) - 0.5f - horizon_shift * 0.65f;
			std::uint8_t r, g, b;
			if (fy < 0.0f)
			{
				const float t = std::clamp(-fy * 2.0f, 0.0f, 1.0f);
				r = static_cast<std::uint8_t>(160.0f * (1.0f - t) + 52.0f * t);
				g = static_cast<std::uint8_t>(188.0f * (1.0f - t) + 108.0f * t);
				b = static_cast<std::uint8_t>(218.0f * (1.0f - t) + 182.0f * t);
			}
			else
			{
				const float t = std::clamp(fy * 2.5f, 0.0f, 1.0f);
				r = static_cast<std::uint8_t>(145.0f * (1.0f - t) + 58.0f * t);
				g = static_cast<std::uint8_t>(155.0f * (1.0f - t) + 64.0f * t);
				b = static_cast<std::uint8_t>(165.0f * (1.0f - t) + 70.0f * t);
			}
			const std::uint32_t col = pack_bgra(r, g, b);
			std::uint32_t* row = color_buf_.data() + static_cast<std::size_t>(y) * width_;
			float* zrow = depth_buf_.data() + static_cast<std::size_t>(y) * width_;
			for (int x = 0; x < width_; ++x)
			{
				row[x] = col;
				zrow[x] = 0.0f; // storing 1/w, larger = closer
			}
		}
	}

	void SoftwareRasterizer::clear_solid_black()
	{
		const std::uint32_t black = pack_bgra(0, 0, 0, 255);
		std::fill(color_buf_.begin(), color_buf_.end(), black);
		std::fill(depth_buf_.begin(), depth_buf_.end(), 0.0f);
	}

	void SoftwareRasterizer::submit_triangle(
		const world::RenderVertex& v0, const world::RenderVertex& v1, const world::RenderVertex& v2,
		const math::Mat4& vp, const math::Vec3& sun_dir, bool fullbright, bool wireframe,
		const assets::DecodedTexture* diffuse, const assets::DecodedTexture* lightmap, bool alpha_test,
		std::vector<ScreenTri>& out_tris)
	{
		auto make_clip_vert = [&](const world::RenderVertex& v) -> ClipVertex {
			const math::Vec4 c = vp.transform({ v.x, v.y, v.z, 1.0f });
			ClipVertex cv{};
			cv.x = c.x; cv.y = c.y; cv.z = c.z; cv.w = c.w;
			cv.u = v.u; cv.v = v.v;
			cv.lu = v.lu; cv.lv = v.lv;
			if (fullbright)
			{
				cv.r = cv.g = cv.b = 1.0f;
			}
			else
			{
				const float ndl = std::max(0.0f, -(v.nx * sun_dir.x + v.ny * sun_dir.y + v.nz * sun_dir.z));
				const float hemi = 0.55f + 0.25f * std::clamp(v.nz, -1.0f, 1.0f);
				const float shade = lightmap ? (0.95f + 0.25f * ndl) : (hemi + 0.55f * ndl);
				const float vr = (v.r > 0 || v.g > 0 || v.b > 0) ? (static_cast<float>(v.r) / 255.0f) : 1.0f;
				const float vg = (v.r > 0 || v.g > 0 || v.b > 0) ? (static_cast<float>(v.g) / 255.0f) : 1.0f;
				const float vb = (v.r > 0 || v.g > 0 || v.b > 0) ? (static_cast<float>(v.b) / 255.0f) : 1.0f;
				cv.r = std::clamp(shade * (0.78f + 0.22f * vr), 0.25f, 1.55f);
				cv.g = std::clamp(shade * (0.78f + 0.22f * vg), 0.25f, 1.55f);
				cv.b = std::clamp(shade * (0.78f + 0.22f * vb), 0.25f, 1.55f);
			}
			return cv;
		};

		ClipVertex in_poly[3] = { make_clip_vert(v0), make_clip_vert(v1), make_clip_vert(v2) };

		// Quick reject if all 3 vertices behind near plane (z + w < 0 or w <= 0)
		constexpr float NEAR_W = 4.0f;
		const bool inside0 = (in_poly[0].w >= NEAR_W) && (in_poly[0].z >= -in_poly[0].w);
		const bool inside1 = (in_poly[1].w >= NEAR_W) && (in_poly[1].z >= -in_poly[1].w);
		const bool inside2 = (in_poly[2].w >= NEAR_W) && (in_poly[2].z >= -in_poly[2].w);
		if (!inside0 && !inside1 && !inside2) return;

		// Quick frustum reject against X/Y clip planes when all inside near
		if (inside0 && inside1 && inside2)
		{
			if ((in_poly[0].x < -in_poly[0].w && in_poly[1].x < -in_poly[1].w && in_poly[2].x < -in_poly[2].w) ||
			    (in_poly[0].x >  in_poly[0].w && in_poly[1].x >  in_poly[1].w && in_poly[2].x >  in_poly[2].w) ||
			    (in_poly[0].y < -in_poly[0].w && in_poly[1].y < -in_poly[1].w && in_poly[2].y < -in_poly[2].w) ||
			    (in_poly[0].y >  in_poly[0].w && in_poly[1].y >  in_poly[1].w && in_poly[2].y >  in_poly[2].w))
			{
				return;
			}
		}

		// Sutherland-Hodgman clip against near plane (w >= NEAR_W)
		ClipVertex clipped[6];
		int count = 0;
		auto lerp_cv = [](const ClipVertex& a, const ClipVertex& b, float t) -> ClipVertex {
			return {
				a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t,
				a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t,
				a.lu + (b.lu - a.lu) * t, a.lv + (b.lv - a.lv) * t,
				a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t
			};
		};

		for (int i = 0; i < 3; ++i)
		{
			const ClipVertex& a = in_poly[i];
			const ClipVertex& b = in_poly[(i + 1) % 3];
			const float da = a.w - NEAR_W;
			const float db = b.w - NEAR_W;
			if (da >= 0.0f) clipped[count++] = a;
			if ((da >= 0.0f) != (db >= 0.0f))
			{
				const float t = da / (da - db);
				clipped[count++] = lerp_cv(a, b, t);
			}
		}
		if (count < 3) return;

		const float half_w = static_cast<float>(width_) * 0.5f;
		const float half_h = static_cast<float>(height_) * 0.5f;

		for (int t = 1; t + 1 < count; ++t)
		{
			const ClipVertex* tv[3] = { &clipped[0], &clipped[t], &clipped[t + 1] };
			ScreenTri st{};
			for (int k = 0; k < 3; ++k)
			{
				const float iw = 1.0f / tv[k]->w;
				st.x[k] = (tv[k]->x * iw + 1.0f) * half_w;
				st.y[k] = (1.0f - tv[k]->y * iw) * half_h;
				st.inv_w[k] = iw;
				st.u_w[k] = tv[k]->u * iw;
				st.v_w[k] = tv[k]->v * iw;
				st.lu_w[k] = tv[k]->lu * iw;
				st.lv_w[k] = tv[k]->lv * iw;
				st.r_w[k] = tv[k]->r * iw;
				st.g_w[k] = tv[k]->g * iw;
				st.b_w[k] = tv[k]->b * iw;
			}

			// Compute screen-space signed area (degenerate check + backface check)
			const float area = edge_fn(st.x[0], st.y[0], st.x[1], st.y[1], st.x[2], st.y[2]);
			if (std::fabs(area) < 0.25f) continue;

			// Normalize winding to positive area for edge-function rasterizer
			// CODOL GfxWorld uses CW front-facing (area < 0 in Y-down screen coords);
			// swap vertices 1 and 2 when area < 0 to render both/front consistently.
			if (area > 0.0f)
			{
				// Backface in CODOL CW winding (skip unless wireframe)
				if (!wireframe) continue;
			}
			else
			{
				std::swap(st.x[1], st.x[2]);
				std::swap(st.y[1], st.y[2]);
				std::swap(st.inv_w[1], st.inv_w[2]);
				std::swap(st.u_w[1], st.u_w[2]);
				std::swap(st.v_w[1], st.v_w[2]);
				std::swap(st.lu_w[1], st.lu_w[2]);
				std::swap(st.lv_w[1], st.lv_w[2]);
				std::swap(st.r_w[1], st.r_w[2]);
				std::swap(st.g_w[1], st.g_w[2]);
				std::swap(st.b_w[1], st.b_w[2]);
			}

			const float minxf = std::min({ st.x[0], st.x[1], st.x[2] });
			const float maxxf = std::max({ st.x[0], st.x[1], st.x[2] });
			const float minyf = std::min({ st.y[0], st.y[1], st.y[2] });
			const float maxyf = std::max({ st.y[0], st.y[1], st.y[2] });

			if (maxxf < 0.0f || minxf >= static_cast<float>(width_) ||
			    maxyf < 0.0f || minyf >= static_cast<float>(height_))
				continue;

			st.min_x = std::max(0, static_cast<int>(std::floor(minxf)));
			st.max_x = std::min(width_ - 1, static_cast<int>(std::ceil(maxxf)));
			st.min_y = std::max(0, static_cast<int>(std::floor(minyf)));
			st.max_y = std::min(height_ - 1, static_cast<int>(std::ceil(maxyf)));
			st.diffuse = diffuse;
			st.lightmap = lightmap;
			st.alpha_test = alpha_test;
			st.wireframe = wireframe;
			out_tris.push_back(st);
		}
	}

	void SoftwareRasterizer::rasterize_band(int y0, int y1, const std::vector<ScreenTri>& tris)
	{
		for (const ScreenTri& tri : tris)
		{
			if (tri.max_y < y0 || tri.min_y >= y1) continue;

			const int sy0 = std::max(tri.min_y, y0);
			const int sy1 = std::min(tri.max_y, y1 - 1);
			const float area = edge_fn(tri.x[0], tri.y[0], tri.x[1], tri.y[1], tri.x[2], tri.y[2]);
			if (area <= 1e-4f) continue;
			const float inv_area = 1.0f / area;

			const float dx01 = tri.x[1] - tri.x[0], dy01 = tri.y[1] - tri.y[0];
			const float dx12 = tri.x[2] - tri.x[1], dy12 = tri.y[2] - tri.y[1];
			const float dx20 = tri.x[0] - tri.x[2], dy20 = tri.y[0] - tri.y[2];

			const auto* tex = (tri.diffuse && tri.diffuse->is_valid()) ? tri.diffuse : nullptr;
			const auto* lmap = (tri.lightmap && tri.lightmap->is_valid()) ? tri.lightmap : nullptr;

			for (int y = sy0; y <= sy1; ++y)
			{
				const float py = static_cast<float>(y) + 0.5f;
				const float px0 = static_cast<float>(tri.min_x) + 0.5f;
				float e0 = (px0 - tri.x[1]) * dy12 - (py - tri.y[1]) * dx12;
				float e1 = (px0 - tri.x[2]) * dy20 - (py - tri.y[2]) * dx20;
				float e2 = (px0 - tri.x[0]) * dy01 - (py - tri.y[0]) * dx01;

				std::uint32_t* crow = color_buf_.data() + static_cast<std::size_t>(y) * width_;
				float* zrow = depth_buf_.data() + static_cast<std::size_t>(y) * width_;

				for (int x = tri.min_x; x <= tri.max_x; ++x)
				{
					if (e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f)
					{
						const float b0 = e0 * inv_area;
						const float b1 = e1 * inv_area;
						const float b2 = e2 * inv_area;

						const float iw = b0 * tri.inv_w[0] + b1 * tri.inv_w[1] + b2 * tri.inv_w[2];
						if (iw > zrow[x])
						{
							const float w = 1.0f / iw;
							float tr = 185.0f, tg = 185.0f, tb = 185.0f;
							std::uint8_t ta = 255;

							if (tex)
							{
								const float u = (b0 * tri.u_w[0] + b1 * tri.u_w[1] + b2 * tri.u_w[2]) * w;
								const float v = (b0 * tri.v_w[0] + b1 * tri.v_w[1] + b2 * tri.v_w[2]) * w;
								const int tx = wrap_texcoord(u, tex->width);
								const int ty = wrap_texcoord(v, tex->height);
								const std::uint8_t* p = &tex->rgba[(static_cast<std::size_t>(ty) * tex->width + tx) * 4];
								tr = static_cast<float>(p[0]);
								tg = static_cast<float>(p[1]);
								tb = static_cast<float>(p[2]);
								ta = p[3];
								if (tri.alpha_test && ta < 128)
								{
									e0 += dy12; e1 += dy20; e2 += dy01;
									continue;
								}
							}

							float lr = (b0 * tri.r_w[0] + b1 * tri.r_w[1] + b2 * tri.r_w[2]) * w;
							float lg = (b0 * tri.g_w[0] + b1 * tri.g_w[1] + b2 * tri.g_w[2]) * w;
							float lb = (b0 * tri.b_w[0] + b1 * tri.b_w[1] + b2 * tri.b_w[2]) * w;

							if (lmap)
							{
								const float lu = (b0 * tri.lu_w[0] + b1 * tri.lu_w[1] + b2 * tri.lu_w[2]) * w;
								const float lv = (b0 * tri.lv_w[0] + b1 * tri.lv_w[1] + b2 * tri.lv_w[2]) * w;
								const int lx = std::clamp(static_cast<int>(lu * lmap->width), 0, lmap->width - 1);
								const int ly = std::clamp(static_cast<int>(lv * lmap->height), 0, lmap->height - 1);
								const std::uint8_t* lp = &lmap->rgba[(static_cast<std::size_t>(ly) * lmap->width + lx) * 4];
								// Lift linear/HDR CODOL lightmap into readable sRGB brightness
								lr *= std::clamp(std::sqrt(static_cast<float>(lp[0]) / 255.0f) * 1.45f, 0.35f, 1.85f);
								lg *= std::clamp(std::sqrt(static_cast<float>(lp[1]) / 255.0f) * 1.45f, 0.35f, 1.85f);
								lb *= std::clamp(std::sqrt(static_cast<float>(lp[2]) / 255.0f) * 1.45f, 0.35f, 1.85f);
							}

							// Distance atmospheric haze (subtle realism past 2400 units)
							const float fog = std::clamp((w - 1800.0f) / 6500.0f, 0.0f, 0.55f);
							const float fr = tr * lr * (1.0f - fog) + 158.0f * fog;
							const float fg = tg * lg * (1.0f - fog) + 176.0f * fog;
							const float fb = tb * lb * (1.0f - fog) + 196.0f * fog;

							zrow[x] = iw;
							crow[x] = pack_bgra(
								static_cast<std::uint8_t>(std::clamp(fr, 0.0f, 255.0f)),
								static_cast<std::uint8_t>(std::clamp(fg, 0.0f, 255.0f)),
								static_cast<std::uint8_t>(std::clamp(fb, 0.0f, 255.0f)));
						}
					}
					e0 += dy12;
					e1 += dy20;
					e2 += dy01;
				}
			}
		}
	}

	void SoftwareRasterizer::draw_line_2d(int x0, int y0, int x1, int y1, std::uint32_t bgra)
	{
		int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
		int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
		int err = dx + dy;
		for (int guard = 0; guard < 4096; ++guard)
		{
			if (x0 >= 0 && x0 < width_ && y0 >= 0 && y0 < height_)
				color_buf_[static_cast<std::size_t>(y0) * width_ + x0] = bgra;
			if (x0 == x1 && y0 == y1) break;
			const int e2 = 2 * err;
			if (e2 >= dy) { err += dy; x0 += sx; }
			if (e2 <= dx) { err += dx; y0 += sy; }
		}
	}

	void SoftwareRasterizer::draw_world_box(const math::Vec3& mins, const math::Vec3& maxs, const math::Mat4& vp, std::uint32_t bgra)
	{
		const math::Vec3 corners[8] = {
			{ mins.x, mins.y, mins.z }, { maxs.x, mins.y, mins.z },
			{ maxs.x, maxs.y, mins.z }, { mins.x, maxs.y, mins.z },
			{ mins.x, mins.y, maxs.z }, { maxs.x, mins.y, maxs.z },
			{ maxs.x, maxs.y, maxs.z }, { mins.x, maxs.y, maxs.z }
		};
		int sx[8], sy[8];
		bool vis[8];
		const float hw = static_cast<float>(width_) * 0.5f;
		const float hh = static_cast<float>(height_) * 0.5f;
		for (int i = 0; i < 8; ++i)
		{
			const auto c = vp.transform({ corners[i].x, corners[i].y, corners[i].z, 1.0f });
			if (c.w <= 24.0f) return; // skip boxes straddling the near plane to avoid projected edge blowup
			const float px = (c.x / c.w + 1.0f) * hw;
			const float py = (1.0f - c.y / c.w) * hh;
			if (px < -width_ || px > width_ * 2 || py < -height_ || py > height_ * 2) return;
			vis[i] = true;
			sx[i] = static_cast<int>(px);
			sy[i] = static_cast<int>(py);
		}
		static constexpr int EDGES[12][2] = {
			{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
		};
		for (const auto& e : EDGES)
		{
			if (vis[e[0]] && vis[e[1]])
				draw_line_2d(sx[e[0]], sy[e[0]], sx[e[1]], sy[e[1]], bgra);
		}
	}

	void SoftwareRasterizer::draw_rect_alpha(int x, int y, int w, int h, std::uint32_t bgra, std::uint8_t alpha)
	{
		const int x0 = std::max(0, x);
		const int y0 = std::max(0, y);
		const int x1 = std::min(width_, x + w);
		const int y1 = std::min(height_, y + h);
		const std::uint32_t sr = (bgra >> 16) & 0xFF;
		const std::uint32_t sg = (bgra >> 8) & 0xFF;
		const std::uint32_t sb = bgra & 0xFF;
		const std::uint32_t inv = 255u - alpha;
		for (int py = y0; py < y1; ++py)
		{
			std::uint32_t* row = color_buf_.data() + static_cast<std::size_t>(py) * width_;
			for (int px = x0; px < x1; ++px)
			{
				const std::uint32_t dst = row[px];
				const std::uint32_t dr = (dst >> 16) & 0xFF;
				const std::uint32_t dg = (dst >> 8) & 0xFF;
				const std::uint32_t db = dst & 0xFF;
				row[px] = pack_bgra(
					static_cast<std::uint8_t>((sr * alpha + dr * inv) / 255u),
					static_cast<std::uint8_t>((sg * alpha + dg * inv) / 255u),
					static_cast<std::uint8_t>((sb * alpha + db * inv) / 255u));
			}
		}
	}

	void SoftwareRasterizer::draw_ui_cmd(const UiDrawCmd& cmd)
	{
		if (cmd.w <= 0.0f || cmd.h <= 0.0f || cmd.a <= 0.0f) return;

		const int x0 = std::max(0, static_cast<int>(std::floor(cmd.x)));
		const int y0 = std::max(0, static_cast<int>(std::floor(cmd.y)));
		const int x1 = std::min(width_, static_cast<int>(std::ceil(cmd.x + cmd.w)));
		const int y1 = std::min(height_, static_cast<int>(std::ceil(cmd.y + cmd.h)));
		if (x0 >= x1 || y0 >= y1) return;

		const auto* tex = (cmd.texture && cmd.texture->is_valid()) ? cmd.texture : nullptr;
		const float inv_w = 1.0f / cmd.w;
		const float inv_h = 1.0f / cmd.h;
		const float du = cmd.u1 - cmd.u0;
		const float dv = cmd.v1 - cmd.v0;

		for (int py = y0; py < y1; ++py)
		{
			std::uint32_t* row = color_buf_.data() + static_cast<std::size_t>(py) * width_;
			const float fy = ((static_cast<float>(py) + 0.5f) - cmd.y) * inv_h;
			const float v = cmd.v0 + fy * dv;

			for (int px = x0; px < x1; ++px)
			{
				float sr = cmd.r * 255.0f;
				float sg = cmd.g * 255.0f;
				float sb = cmd.b * 255.0f;
				float sa = cmd.a;

				if (tex)
				{
					const float fx = ((static_cast<float>(px) + 0.5f) - cmd.x) * inv_w;
					const float u = cmd.u0 + fx * du;
					const float tx = std::clamp(u * static_cast<float>(tex->width) - 0.5f, 0.0f, static_cast<float>(tex->width - 1));
					const float ty = std::clamp(v * static_cast<float>(tex->height) - 0.5f, 0.0f, static_cast<float>(tex->height - 1));
					const int ix0 = static_cast<int>(std::floor(tx));
					const int iy0 = static_cast<int>(std::floor(ty));
					const int ix1 = std::min(tex->width - 1, ix0 + 1);
					const int iy1 = std::min(tex->height - 1, iy0 + 1);
					const float wx = tx - static_cast<float>(ix0);
					const float wy = ty - static_cast<float>(iy0);

					auto sample_pm = [&](int sx, int sy, float& pr, float& pg, float& pb, float& pa) {
						const std::uint8_t* p = &tex->rgba[(static_cast<std::size_t>(sy) * tex->width + sx) * 4];
						const float a = static_cast<float>(p[3]) * (1.0f / 255.0f);
						pr = static_cast<float>(p[0]) * a;
						pg = static_cast<float>(p[1]) * a;
						pb = static_cast<float>(p[2]) * a;
						pa = a;
					};
					float r00, g00, b00, a00, r10, g10, b10, a10;
					float r01, g01, b01, a01, r11, g11, b11, a11;
					sample_pm(ix0, iy0, r00, g00, b00, a00);
					sample_pm(ix1, iy0, r10, g10, b10, a10);
					sample_pm(ix0, iy1, r01, g01, b01, a01);
					sample_pm(ix1, iy1, r11, g11, b11, a11);

					const float w00 = (1.0f - wx) * (1.0f - wy);
					const float w10 = wx * (1.0f - wy);
					const float w01 = (1.0f - wx) * wy;
					const float w11 = wx * wy;
					const float pa = a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11;
					const float pr = r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11;
					const float pg = g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11;
					const float pb = b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11;

					if (cmd.blend == UiBlendMode::Opaque)
					{
						const float inv_a = pa > 1e-4f ? (1.0f / pa) : 0.0f;
						sr = pr * inv_a * cmd.r;
						sg = pg * inv_a * cmd.g;
						sb = pb * inv_a * cmd.b;
						sa = 1.0f;
					}
					else
					{
						if (pa <= 1e-4f) continue;
						const float inv_a = 1.0f / pa;
						sr = pr * inv_a * cmd.r;
						sg = pg * inv_a * cmd.g;
						sb = pb * inv_a * cmd.b;
						sa = std::clamp(pa * cmd.a, 0.0f, 1.0f);
					}
				}
				else
				{
					sa = std::clamp(sa, 0.0f, 1.0f);
				}

				if (sa <= 1e-4f) continue;

				const std::uint32_t dst = row[px];
				const float dr = static_cast<float>((dst >> 16) & 0xFF);
				const float dg = static_cast<float>((dst >> 8) & 0xFF);
				const float db = static_cast<float>(dst & 0xFF);
				float or_, og_, ob_;
				if (cmd.blend == UiBlendMode::Opaque)
				{
					or_ = sr; og_ = sg; ob_ = sb;
				}
				else if (cmd.blend == UiBlendMode::Additive)
				{
					or_ = dr + sr * sa;
					og_ = dg + sg * sa;
					ob_ = db + sb * sa;
				}
				else
				{
					const float inv = 1.0f - sa;
					or_ = sr * sa + dr * inv;
					og_ = sg * sa + dg * inv;
					ob_ = sb * sa + db * inv;
				}
				row[px] = pack_bgra(
					static_cast<std::uint8_t>(std::clamp(or_, 0.0f, 255.0f)),
					static_cast<std::uint8_t>(std::clamp(og_, 0.0f, 255.0f)),
					static_cast<std::uint8_t>(std::clamp(ob_, 0.0f, 255.0f)));
			}
		}
	}

	void SoftwareRasterizer::draw_text(int x, int y, const std::string& text, std::uint32_t bgra, int scale)
	{
		int cx = x;
		for (char ch : text)
		{
			const std::uint8_t* rows = glyph_8x8(ch);
			for (int r = 0; r < 8; ++r)
			{
				const std::uint8_t bits = rows[r];
				for (int c = 0; c < 8; ++c)
				{
					if ((bits & (1u << c)) == 0) continue;
					for (int sy = 0; sy < scale; ++sy)
					{
						const int py = y + r * scale + sy;
						if (py < 0 || py >= height_) continue;
						for (int sx = 0; sx < scale; ++sx)
						{
							const int px = cx + c * scale + sx;
							if (px >= 0 && px < width_)
								color_buf_[static_cast<std::size_t>(py) * width_ + px] = bgra;
						}
					}
				}
			}
			cx += 8 * scale;
		}
	}

	void SoftwareRasterizer::draw_hud(const world::WorldScene* scene, const RenderFrameParams& params, const platform::PerfMetrics& metrics)
	{
		// 1. Authentic client 2D UI layers (splash.bmp, IW menuDef_t, Scaleform GFx sprites, Font_s)
		if (params.ui_cmds)
		{
			for (const auto& cmd : *params.ui_cmds)
				draw_ui_cmd(cmd);
		}

		// 2. Explicit developer overlays (all OFF by default — never shown during normal authentic startup/gameplay)
		if (params.show_dev_crosshair && !params.show_dev_map_menu)
		{
			const int cx = width_ / 2;
			const int cy = height_ / 2;
			const std::uint32_t ch_col = pack_bgra(235, 255, 220);
			draw_line_2d(cx - 10, cy, cx - 3, cy, ch_col);
			draw_line_2d(cx + 3, cy, cx + 10, cy, ch_col);
			draw_line_2d(cx, cy - 10, cx, cy - 3, ch_col);
			draw_line_2d(cx, cy + 3, cx, cy + 10, ch_col);
		}

		if (params.show_dev_hud)
		{
			const int scale = width_ >= 1600 ? 2 : 1;
			const int lh = 11 * scale;
			draw_rect_alpha(8, 8, width_ - 16, lh * 3 + 12, pack_bgra(10, 14, 20), 175);
			char line1[256];
			std::snprintf(line1, sizeof(line1),
				"[DEV HUD] MAP: %s (%s) | GPU: %s",
				params.map_name.empty() ? "NONE" : params.map_name.c_str(),
				params.gametype.c_str(),
				metrics.active_gpu_backend.c_str());
			draw_text(16, 14, line1, pack_bgra(255, 190, 60), scale);

			char line2[256];
			std::snprintf(line2, sizeof(line2),
				"FPS: %.1f (%.1f ms) | SURFS: %u | TRIS: %u | POS: %.0f, %.0f, %.0f | YAW: %.0f",
				metrics.fps, metrics.frame_ms, metrics.visible_surfaces, metrics.drawn_triangles,
				params.eye.x, params.eye.y, params.eye.z, params.yaw_deg);
			draw_text(16, 14 + lh, line2, pack_bgra(220, 240, 255), scale);

			char line3[256];
			if (scene)
			{
				std::snprintf(line3, sizeof(line3),
					"WORLD: %u verts, %u/%u textured surfs, %u lightmaps, %u props | [L1+R1+TOUCHPAD] Dev Map Selector",
					scene->raw_world_vertex_count, scene->resolved_textured_surfaces, scene->raw_surface_count,
					static_cast<unsigned>(scene->lightmaps.size()), scene->decoded_static_model_instances);
			}
			else
			{
				std::snprintf(line3, sizeof(line3), "No 3D world loaded | Place CODOL 1.1.1.13 files in /data/codol or /usb0/codol");
			}
			draw_text(16, 14 + lh * 2, line3, pack_bgra(165, 225, 175), scale);

			char wline[128];
			std::snprintf(wline, sizeof(wline), "[DEV] WPN: %s%s", params.weapon_name.c_str(), params.reloading ? " [RELOADING]" : "");
			char aline[128];
			std::snprintf(aline, sizeof(aline), "AMMO: %d / %d  |  HP: %d  |  SCORE: %d",
				params.ammo_clip, params.ammo_reserve, params.health, params.score);
			const int max_chars = static_cast<int>(std::max(std::strlen(wline), std::strlen(aline)));
			const int bw = std::min(width_ - 24, std::max(280 * scale, max_chars * 8 * scale + 24));
			const int bh = lh * 2 + 16;
			draw_rect_alpha(width_ - bw - 12, height_ - bh - 12, bw, bh, pack_bgra(10, 14, 20), 175);
			draw_text(width_ - bw - 2, height_ - bh - 4, wline, pack_bgra(255, 235, 180), scale);
			draw_text(width_ - bw - 2, height_ - bh - 4 + lh, aline, pack_bgra(255, 255, 255), scale);
		}

		if (params.show_console)
		{
			const int ch = height_ / 2;
			draw_rect_alpha(12, 76, width_ - 24, ch, pack_bgra(8, 10, 14), 220);
			draw_text(20, 82, "=== [DEV TOOL] DEBUG CONSOLE ===", pack_bgra(255, 160, 40), 1);
			int y = 98;
			for (const auto& l : params.console_lines)
			{
				if (y + 10 >= 76 + ch) break;
				draw_text(20, y, l, pack_bgra(210, 235, 210), 1);
				y += 11;
			}
		}

		if (params.show_dev_map_menu && !params.menu_items.empty())
		{
			const int mw = std::min(width_ - 64, 560);
			const int mh = std::min(height_ - 120, 380);
			const int mx = (width_ - mw) / 2;
			const int my = (height_ - mh) / 2;
			draw_rect_alpha(mx, my, mw, mh, pack_bgra(12, 16, 24), 235);
			draw_text(mx + 16, my + 12, "[DEV TOOL] MAP SELECTOR (D-PAD + X, CIRCLE=CLOSE)", pack_bgra(255, 195, 50), 1);
			const int visible_rows = (mh - 42) / 14;
			const int start_row = std::max(0, params.menu_selected - visible_rows / 2);
			for (int i = 0; i < visible_rows && (start_row + i) < static_cast<int>(params.menu_items.size()); ++i)
			{
				const int idx = start_row + i;
				const bool sel = (idx == params.menu_selected);
				if (sel) draw_rect_alpha(mx + 10, my + 30 + i * 14, mw - 20, 13, pack_bgra(210, 110, 20), 200);
				draw_text(mx + 18, my + 33 + i * 14, params.menu_items[static_cast<std::size_t>(idx)],
					sel ? pack_bgra(255, 255, 255) : pack_bgra(200, 210, 225), 1);
			}
		}
	}

	void SoftwareRasterizer::render(const world::WorldScene* scene, const RenderFrameParams& params, platform::PerfMetrics& metrics)
	{
		const std::uint64_t t_start = platform::time_now_ns();
		frame_tris_.clear();
		metrics.visible_surfaces = 0;
		metrics.drawn_triangles = 0;
		metrics.drawn_models = 0;
		metrics.cull_ms = 0.0;

		if (!params.render_3d_scene || !scene || scene->vertices.empty())
		{
			clear_solid_black();
			draw_hud(scene, params, metrics);
			const std::uint64_t t_end = platform::time_now_ns();
			metrics.render_ms = static_cast<double>(t_end - t_start) * 1e-6;
			metrics.frame_ms = metrics.render_ms;
			return;
		}

		clear_sky_and_depth(params.pitch_deg);

		math::Vec3 fwd, right, up;
		math::angles_to_basis(params.yaw_deg, params.pitch_deg, fwd, right, up);
		const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
		const math::Mat4 view = math::Mat4::look_at(params.eye, params.eye + fwd, { 0.0f, 0.0f, 1.0f });
		const math::Mat4 proj = math::Mat4::perspective(params.fov_deg, aspect, 4.0f, 18000.0f);
		const math::Mat4 vp = math::Mat4::multiply(proj, view);
		const math::Frustum frustum = math::Frustum::from_view_proj(vp);

		const std::uint64_t t_cull0 = platform::time_now_ns();
		{
			std::vector<const world::WorldSurface*> vis_world;
			std::vector<const world::StaticModelMesh*> vis_models;
			scene->collect_visible_surfaces(frustum, params.eye, params.frustum_cull, params.draw_static_models, vis_world, vis_models);

			metrics.visible_surfaces = static_cast<std::uint32_t>(vis_world.size());
			metrics.drawn_models = static_cast<std::uint32_t>(vis_models.size());
			const math::Vec3 sun_dir = scene->has_sun ? scene->sun_light.direction : math::normalize({ -0.45f, -0.35f, -0.82f });

			for (const world::WorldSurface* s : vis_world)
			{
				if (s->is_sky) continue; // CoD skyboxes use 3D view-ray cubemap shaders, not planar 2D UVs
				const assets::DecodedTexture* diff = (s->material && s->material->color_texture) ? s->material->color_texture.get() : nullptr;
				const assets::DecodedTexture* lmap = (params.use_lightmap && s->lightmap_index < scene->lightmaps.size())
					? scene->lightmaps[s->lightmap_index].get() : nullptr;
				const bool alpha_test = diff && diff->is_alpha_tested;

				for (std::uint32_t t = 0; t < s->tri_count; ++t)
				{
					const std::uint32_t i0 = scene->indices[s->first_index + t * 3 + 0];
					const std::uint32_t i1 = scene->indices[s->first_index + t * 3 + 1];
					const std::uint32_t i2 = scene->indices[s->first_index + t * 3 + 2];
					submit_triangle(scene->vertices[i0], scene->vertices[i1], scene->vertices[i2],
						vp, sun_dir, params.fullbright || s->is_sky || s->emissive, params.wireframe,
						diff, s->is_sky ? nullptr : lmap, alpha_test, frame_tris_);
				}
			}

			for (const world::StaticModelMesh* m : vis_models)
			{
				const assets::DecodedTexture* diff = (m->material && m->material->color_texture) ? m->material->color_texture.get() : nullptr;
				const bool alpha_test = diff && diff->is_alpha_tested;
				for (std::uint32_t t = 0; t < m->tri_count; ++t)
				{
					const std::uint32_t i0 = scene->indices[m->first_index + t * 3 + 0];
					const std::uint32_t i1 = scene->indices[m->first_index + t * 3 + 1];
					const std::uint32_t i2 = scene->indices[m->first_index + t * 3 + 2];
					submit_triangle(scene->vertices[i0], scene->vertices[i1], scene->vertices[i2],
						vp, sun_dir, params.fullbright, params.wireframe, diff, nullptr, alpha_test, frame_tris_);
				}
			}
		}
		const std::uint64_t t_cull1 = platform::time_now_ns();
		metrics.cull_ms = static_cast<double>(t_cull1 - t_cull0) * 1e-6;
		metrics.drawn_triangles = static_cast<std::uint32_t>(frame_tris_.size());

		// Parallel horizontal band rasterization
		constexpr int NUM_BANDS = 16;
		const int band_h = (height_ + NUM_BANDS - 1) / NUM_BANDS;
		if (pool_)
		{
			pool_->parallel_for(NUM_BANDS, [&](int band) {
				const int y0 = band * band_h;
				const int y1 = std::min(height_, y0 + band_h);
				if (y0 < y1) rasterize_band(y0, y1, frame_tris_);
			});
		}
		else
		{
			rasterize_band(0, height_, frame_tris_);
		}

		// Optional wireframe edges
		if (params.wireframe)
		{
			const std::uint32_t wcol = pack_bgra(60, 255, 120);
			for (const auto& tri : frame_tris_)
			{
				draw_line_2d(static_cast<int>(tri.x[0]), static_cast<int>(tri.y[0]), static_cast<int>(tri.x[1]), static_cast<int>(tri.y[1]), wcol);
				draw_line_2d(static_cast<int>(tri.x[1]), static_cast<int>(tri.y[1]), static_cast<int>(tri.x[2]), static_cast<int>(tri.y[2]), wcol);
				draw_line_2d(static_cast<int>(tri.x[2]), static_cast<int>(tri.y[2]), static_cast<int>(tri.x[0]), static_cast<int>(tri.y[0]), wcol);
			}
		}

		// Explicit dev debug boxes for bots & impacts (OFF by default — not authentic character XModels)
		if (params.show_dev_boxes)
		{
			if (params.bots)
			{
				for (const auto& bot : *params.bots)
				{
					if (!bot.alive) continue;
					const std::uint32_t col = bot.team == 0 ? pack_bgra(60, 180, 255) : pack_bgra(255, 70, 60);
					draw_world_box(bot.origin + math::Vec3{ -14, -14, 0 }, bot.origin + math::Vec3{ 14, 14, 68 }, vp, col);
				}
			}
			if (params.impacts)
			{
				const std::uint32_t scol = pack_bgra(255, 220, 60);
				for (const auto& imp : *params.impacts)
					draw_world_box(imp.pos - math::Vec3{ 4, 4, 4 }, imp.pos + math::Vec3{ 4, 4, 4 }, vp, scol);
			}
		}

		const std::uint64_t t_rast1 = platform::time_now_ns();
		metrics.render_ms = static_cast<double>(t_rast1 - t_cull1) * 1e-6;
		metrics.frame_ms = static_cast<double>(t_rast1 - t_start) * 1e-6;

		draw_hud(scene, params, metrics);
	}

	void SoftwareRasterizer::blit_to_bgra(std::uint32_t* dst, int dst_w, int dst_h, int dst_pitch_pixels) const
	{
		if (!dst || color_buf_.empty() || dst_w <= 0 || dst_h <= 0) return;
		if (dst_w == width_ && dst_h == height_)
		{
			for (int y = 0; y < dst_h; ++y)
			{
				std::memcpy(dst + static_cast<std::size_t>(y) * dst_pitch_pixels,
					color_buf_.data() + static_cast<std::size_t>(y) * width_,
					static_cast<std::size_t>(width_) * 4);
			}
			return;
		}
		// Fast nearest upscale (exact 2x when 960x540 -> 1920x1080)
		for (int y = 0; y < dst_h; ++y)
		{
			const int sy = (y * height_) / dst_h;
			const std::uint32_t* srow = color_buf_.data() + static_cast<std::size_t>(sy) * width_;
			std::uint32_t* drow = dst + static_cast<std::size_t>(y) * dst_pitch_pixels;
			for (int x = 0; x < dst_w; ++x)
			{
				const int sx = (x * width_) / dst_w;
				drow[x] = srow[sx];
			}
		}
	}

	bool SoftwareRasterizer::save_bmp(const std::string& path) const
	{
		if (color_buf_.empty()) return false;
		const std::uint32_t row_bytes = static_cast<std::uint32_t>(width_) * 3u;
		const std::uint32_t pad = (4u - (row_bytes & 3u)) & 3u;
		const std::uint32_t stride = row_bytes + pad;
		const std::uint32_t img_bytes = stride * static_cast<std::uint32_t>(height_);
		const std::uint32_t file_size = 54u + img_bytes;

		std::uint8_t hdr[54]{};
		hdr[0] = 'B'; hdr[1] = 'M';
		auto wr32 = [](std::uint8_t* p, std::uint32_t v) {
			p[0] = static_cast<std::uint8_t>(v);
			p[1] = static_cast<std::uint8_t>(v >> 8);
			p[2] = static_cast<std::uint8_t>(v >> 16);
			p[3] = static_cast<std::uint8_t>(v >> 24);
		};
		wr32(hdr + 2, file_size);
		wr32(hdr + 10, 54);
		wr32(hdr + 14, 40);
		wr32(hdr + 18, static_cast<std::uint32_t>(width_));
		wr32(hdr + 22, static_cast<std::uint32_t>(height_));
		hdr[26] = 1;
		hdr[28] = 24;
		wr32(hdr + 34, img_bytes);

		std::FILE* f = std::fopen(path.c_str(), "wb");
		if (!f) return false;
		std::fwrite(hdr, 1, 54, f);
		std::vector<std::uint8_t> row(stride, 0);
		for (int y = height_ - 1; y >= 0; --y)
		{
			const std::uint32_t* src = color_buf_.data() + static_cast<std::size_t>(y) * width_;
			for (int x = 0; x < width_; ++x)
			{
				const std::uint32_t p = src[x];
				row[static_cast<std::size_t>(x) * 3 + 0] = static_cast<std::uint8_t>(p & 0xFF);         // B
				row[static_cast<std::size_t>(x) * 3 + 1] = static_cast<std::uint8_t>((p >> 8) & 0xFF);  // G
				row[static_cast<std::size_t>(x) * 3 + 2] = static_cast<std::uint8_t>((p >> 16) & 0xFF); // R
			}
			std::fwrite(row.data(), 1, stride, f);
		}
		std::fclose(f);
		return true;
	}

	bool SoftwareRasterizer::save_ppm(const std::string& path) const
	{
		if (color_buf_.empty()) return false;
		std::FILE* f = std::fopen(path.c_str(), "wb");
		if (!f) return false;
		std::fprintf(f, "P6\n%d %d\n255\n", width_, height_);
		std::vector<std::uint8_t> row(static_cast<std::size_t>(width_) * 3);
		for (int y = 0; y < height_; ++y)
		{
			const std::uint32_t* src = color_buf_.data() + static_cast<std::size_t>(y) * width_;
			for (int x = 0; x < width_; ++x)
			{
				const std::uint32_t p = src[x];
				row[static_cast<std::size_t>(x) * 3 + 0] = static_cast<std::uint8_t>((p >> 16) & 0xFF);
				row[static_cast<std::size_t>(x) * 3 + 1] = static_cast<std::uint8_t>((p >> 8) & 0xFF);
				row[static_cast<std::size_t>(x) * 3 + 2] = static_cast<std::uint8_t>(p & 0xFF);
			}
			std::fwrite(row.data(), 1, row.size(), f);
		}
		std::fclose(f);
		return true;
	}
}
