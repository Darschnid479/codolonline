#include "authentic_ui.hpp"
#include "../platform/platform.hpp"
#include "../world/entities.hpp"

#include "codol_zone/assets.hpp"
#include "codol_zone/c1_layouts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace dragon::ui
{
	namespace
	{
#pragma pack(push, 1)
		struct FontHeaderC1
		{
			codol::c1::zptr fontName;
			std::int32_t pixelHeight;
			std::int32_t glyphCount;
			codol::c1::zptr material;
			codol::c1::zptr glowMaterial;
			codol::c1::zptr glyphs;
		};
		static_assert(sizeof(FontHeaderC1) == 0x18);

		struct GlyphC1
		{
			std::uint16_t letter;
			std::int8_t x0;
			std::int8_t y0;
			std::uint8_t dx;
			std::uint8_t pixelWidth;
			std::uint8_t pixelHeight;
			std::uint8_t pad;
			float s0, t0, s1, t1;
		};
		static_assert(sizeof(GlyphC1) == 24);

		struct RectDefC1
		{
			float x, y, w, h;
			std::uint8_t horizAlign;
			std::uint8_t vertAlign;
			std::uint8_t pad[2];
		};
		static_assert(sizeof(RectDefC1) == 20);

		struct WindowDefC1
		{
			codol::c1::zptr name;          // 0x00
			RectDefC1 rect;                // 0x04
			RectDefC1 rectClient;          // 0x18
			codol::c1::zptr group;         // 0x2C
			std::int32_t style;            // 0x30
			std::int32_t border;           // 0x34
			std::int32_t ownerDraw;        // 0x38
			std::int32_t ownerDrawFlags;   // 0x3C
			float borderSize;              // 0x40
			std::int32_t staticFlags;      // 0x44
			std::int32_t dynamicFlags[1];  // 0x48
			std::int32_t nextTime;         // 0x4C
			float foreColor[4];            // 0x50
			float backColor[4];            // 0x60
			float borderColor[4];          // 0x70
			float outlineColor[4];         // 0x80
			float disableColor[4];         // 0x90
			codol::c1::zptr background;    // 0xA0
		};
		static_assert(sizeof(WindowDefC1) == 0xA4);
#pragma pack(pop)

		std::uint16_t rd_u16(const std::uint8_t* p)
		{
			return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
		}

		std::uint32_t rd_u32(const std::uint8_t* p)
		{
			return static_cast<std::uint32_t>(p[0]) |
			       (static_cast<std::uint32_t>(p[1]) << 8) |
			       (static_cast<std::uint32_t>(p[2]) << 16) |
			       (static_cast<std::uint32_t>(p[3]) << 24);
		}

		std::int32_t rd_i32(const std::uint8_t* p)
		{
			return static_cast<std::int32_t>(rd_u32(p));
		}

		struct BitReader
		{
			const std::uint8_t* data = nullptr;
			std::size_t size = 0;
			std::size_t bit_pos = 0;

			bool has_bits(std::size_t n) const { return (bit_pos + n) <= (size * 8); }

			std::uint32_t u(std::size_t n)
			{
				if (n == 0 || !has_bits(n)) return 0;
				std::uint32_t v = 0;
				for (std::size_t i = 0; i < n; ++i)
				{
					const std::uint8_t byte = data[bit_pos >> 3];
					const std::uint8_t bit = static_cast<std::uint8_t>((byte >> (7 - (bit_pos & 7))) & 1u);
					v = (v << 1) | bit;
					++bit_pos;
				}
				return v;
			}

			std::int32_t s(std::size_t n)
			{
				if (n == 0) return 0;
				std::uint32_t v = u(n);
				if (n < 32 && (v & (1u << (n - 1))))
					v |= (~0u << n);
				return static_cast<std::int32_t>(v);
			}

			void align_byte()
			{
				bit_pos = (bit_pos + 7u) & ~std::size_t(7u);
			}

			std::size_t byte_pos() const { return bit_pos >> 3; }
		};

		bool parse_swf_matrix(const std::uint8_t* buf, std::size_t len, std::size_t& byte_off,
			float& tx, float& ty, float& sx, float& sy)
		{
			tx = ty = 0.0f;
			sx = sy = 1.0f;
			if (byte_off >= len) return false;
			BitReader br{ buf, len, byte_off * 8 };
			if (!br.has_bits(1)) return false;
			if (br.u(1))
			{
				if (!br.has_bits(5)) return false;
				const std::uint32_t nb = br.u(5);
				if (!br.has_bits(nb * 2)) return false;
				sx = static_cast<float>(br.s(nb)) / 65536.0f;
				sy = static_cast<float>(br.s(nb)) / 65536.0f;
			}
			if (!br.has_bits(1)) return false;
			if (br.u(1))
			{
				if (!br.has_bits(5)) return false;
				const std::uint32_t nb = br.u(5);
				if (!br.has_bits(nb * 2)) return false;
				(void)br.s(nb);
				(void)br.s(nb);
			}
			if (!br.has_bits(5)) return false;
			const std::uint32_t nb = br.u(5);
			if (!br.has_bits(nb * 2)) return false;
			tx = static_cast<float>(br.s(nb)) / 20.0f;
			ty = static_cast<float>(br.s(nb)) / 20.0f;
			br.align_byte();
			byte_off = br.byte_pos();
			return true;
		}

		bool parse_swf_cxform_alpha(const std::uint8_t* buf, std::size_t len, std::size_t& byte_off,
			float& mr, float& mg, float& mb, float& ma)
		{
			mr = mg = mb = ma = 1.0f;
			if (byte_off >= len) return false;
			BitReader br{ buf, len, byte_off * 8 };
			if (!br.has_bits(6)) return false;
			const std::uint32_t has_add = br.u(1);
			const std::uint32_t has_mul = br.u(1);
			const std::uint32_t nb = br.u(4);
			if (has_mul)
			{
				if (!br.has_bits(nb * 4)) return false;
				mr = std::clamp(static_cast<float>(br.s(nb)) / 256.0f, 0.0f, 4.0f);
				mg = std::clamp(static_cast<float>(br.s(nb)) / 256.0f, 0.0f, 4.0f);
				mb = std::clamp(static_cast<float>(br.s(nb)) / 256.0f, 0.0f, 4.0f);
				ma = std::clamp(static_cast<float>(br.s(nb)) / 256.0f, 0.0f, 1.0f);
			}
			if (has_add)
			{
				if (!br.has_bits(nb * 4)) return false;
				for (int i = 0; i < 4; ++i) (void)br.s(nb);
			}
			br.align_byte();
			byte_off = br.byte_pos();
			return true;
		}

		bool parse_swf_place_object(std::uint16_t tag, const std::uint8_t* p, std::size_t len, bool gfx_movie, ScaleformPlacement& out)
		{
			if (len < 3) return false;
			std::size_t pos = 0;
			const std::uint8_t fl = p[pos++];
			std::uint8_t fl2 = 0;
			if (tag == 70)
			{
				if (pos >= len) return false;
				fl2 = p[pos++];
			}
			if (pos + 2 > len) return false;
			out.depth = rd_u16(p + pos);
			pos += 2;

			const bool has_char = (fl & 0x02) != 0;
			const bool has_matrix = (fl & 0x04) != 0;
			const bool has_cxform = (fl & 0x08) != 0;
			const bool has_ratio = (fl & 0x10) != 0;
			const bool has_name = (fl & 0x20) != 0;
			const bool has_clip = (fl & 0x40) != 0;

			// GFx external image placements store the character ID directly after depth.
			if (tag == 70 && ((fl2 & 0x08) || (!gfx_movie && (fl2 & 0x10) && has_char)))
			{
				const std::size_t start = pos;
				while (pos < len && p[pos] != 0) ++pos;
				if (pos >= len) return false;
				out.class_name.assign(reinterpret_cast<const char*>(p + start), pos - start);
				++pos;
			}
			if (has_char)
			{
				if (pos + 2 > len) return false;
				out.has_char = true;
				out.char_id = rd_u16(p + pos);
				pos += 2;
			}
			if (has_matrix)
			{
				out.has_matrix = true;
				if (!parse_swf_matrix(p, len, pos, out.tx, out.ty, out.sx, out.sy))
					return false;
			}
			if (has_cxform)
			{
				out.has_cxform = true;
				if (!parse_swf_cxform_alpha(p, len, pos, out.mul_r, out.mul_g, out.mul_b, out.mul_a))
					return false;
			}
			if (has_ratio)
			{
				if (pos + 2 > len) return false;
				pos += 2;
			}
			if (has_name)
			{
				const std::size_t start = pos;
				while (pos < len && p[pos] != 0) ++pos;
				if (pos >= len) return false;
				out.instance_name.assign(reinterpret_cast<const char*>(p + start), pos - start);
				++pos;
			}
			if (has_clip)
			{
				if (pos + 2 > len) return false;
				out.is_clip_mask = true;
				pos += 2;
			}
			if (tag == 70 && (fl2 & 0x02) && !(fl2 & 0x01) && pos < len)
			{
				out.blend_mode = p[pos];
			}
			return true;
		}

		// IW4 / C1 ScrPlace_ApplyRect coordinate conversion from 640x480 virtual space to target framebuffer
		void apply_iw_rect(float rx, float ry, float rw, float rh,
			std::uint8_t h_align, std::uint8_t v_align,
			int fb_w, int fb_h,
			float& ox, float& oy, float& ow, float& oh)
		{
			const float W = static_cast<float>(fb_w);
			const float H = static_cast<float>(fb_h);
			const float scale = H / 480.0f;
			const float virt_w = 640.0f * scale;
			const float subleft = (W - virt_w) * 0.5f;

			if (h_align == 4) // FULLSCREEN
			{
				ox = (rx / 640.0f) * W;
				ow = (rw / 640.0f) * W;
			}
			else
			{
				float base_x = subleft;
				switch (h_align)
				{
				case 0: base_x = subleft; break;
				case 1: base_x = 0.0f; break;
				case 2: case 7: base_x = W * 0.5f; break;
				case 3: case 10: base_x = W; break;
				case 8: base_x = W * 0.05f; break;
				case 9: base_x = W * 0.95f; break;
				default: base_x = subleft; break;
				}
				ox = base_x + rx * scale;
				ow = rw * scale;
			}

			if (v_align == 4) // FULLSCREEN
			{
				oy = (ry / 480.0f) * H;
				oh = (rh / 480.0f) * H;
			}
			else
			{
				float base_y = 0.0f;
				switch (v_align)
				{
				case 0: case 1: base_y = 0.0f; break;
				case 2: case 7: base_y = H * 0.5f; break;
				case 3: case 10: base_y = H; break;
				case 8: base_y = H * 0.05f; break;
				case 9: base_y = H * 0.95f; break;
				default: base_y = 0.0f; break;
				}
				oy = base_y + ry * scale;
				oh = rh * scale;
			}
		}

		std::string join_path(const std::string& a, const std::string& b)
		{
			if (a.empty()) return b;
			if (a.back() == '/' || a.back() == '\\') return a + b;
			return a + "/" + b;
		}
	}

	std::shared_ptr<assets::DecodedTexture> load_bmp_texture(const std::string& path, const std::string& name, std::string* error)
	{
		std::FILE* f = std::fopen(path.c_str(), "rb");
		if (!f)
		{
			if (error) *error = "File not found: " + path;
			return nullptr;
		}
		std::fseek(f, 0, SEEK_END);
		const long sz = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		if (sz < 54)
		{
			std::fclose(f);
			if (error) *error = "BMP too small: " + path;
			return nullptr;
		}
		std::vector<std::uint8_t> data(static_cast<std::size_t>(sz));
		const std::size_t nread = std::fread(data.data(), 1, data.size(), f);
		std::fclose(f);
		if (nread != data.size() || data[0] != 'B' || data[1] != 'M')
		{
			if (error) *error = "Invalid BMP signature: " + path;
			return nullptr;
		}

		const std::uint32_t px_off = rd_u32(data.data() + 10);
		const std::int32_t w = rd_i32(data.data() + 18);
		const std::int32_t h_signed = rd_i32(data.data() + 22);
		const std::uint16_t bpp = rd_u16(data.data() + 28);
		const std::uint32_t comp = rd_u32(data.data() + 30);
		const std::int32_t h = h_signed < 0 ? -h_signed : h_signed;

		if (w <= 0 || h <= 0 || comp != 0 || (bpp != 24 && bpp != 32))
		{
			if (error) *error = "Unsupported BMP format (expected uncompressed 24/32bpp): " + path;
			return nullptr;
		}

		const std::size_t bpp_bytes = bpp / 8u;
		const std::size_t stride = ((static_cast<std::size_t>(w) * bpp_bytes) + 3u) & ~std::size_t(3u);
		if (px_off + stride * static_cast<std::size_t>(h) > data.size())
		{
			if (error) *error = "Truncated BMP pixel data: " + path;
			return nullptr;
		}

		auto tex = std::make_shared<assets::DecodedTexture>();
		tex->name = name;
		tex->width = w;
		tex->height = h;
		tex->rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);

		for (int y = 0; y < h; ++y)
		{
			const int src_y = (h_signed > 0) ? (h - 1 - y) : y;
			const std::uint8_t* src_row = data.data() + px_off + static_cast<std::size_t>(src_y) * stride;
			std::uint8_t* dst_row = tex->rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4u;
			for (int x = 0; x < w; ++x)
			{
				const std::uint8_t* sp = src_row + static_cast<std::size_t>(x) * bpp_bytes;
				std::uint8_t* dp = dst_row + static_cast<std::size_t>(x) * 4u;
				dp[0] = sp[2]; // R
				dp[1] = sp[1]; // G
				dp[2] = sp[0]; // B
				dp[3] = (bpp == 32) ? sp[3] : 255u;
			}
		}
		return tex;
	}

	Bink2Info inspect_bink2_file(const std::string& full_path, const std::string& rel_path)
	{
		Bink2Info info{};
		info.rel_path = rel_path;
		std::FILE* f = std::fopen(full_path.c_str(), "rb");
		if (!f) return info;
		info.found = true;
		std::uint8_t hdr[36]{};
		const std::size_t n = std::fread(hdr, 1, sizeof(hdr), f);
		std::fclose(f);
		if (n < sizeof(hdr)) return info;
		std::memcpy(info.magic, hdr, 4);
		info.magic[4] = '\0';
		if (hdr[0] == 'K' && hdr[1] == 'B' && hdr[2] == '2')
		{
			info.valid_header = true;
			info.frame_count = rd_u32(hdr + 8);
			info.width = rd_u32(hdr + 20);
			info.height = rd_u32(hdr + 24);
			info.fps_num = rd_u32(hdr + 28);
			info.fps_den = std::max<std::uint32_t>(1u, rd_u32(hdr + 32));
		}
		return info;
	}

	void IwFont::emit_text(const std::string& text, float x, float y, float scale,
		float r, float g, float b, float a, std::vector<gfx::UiDrawCmd>& out, int* missing_glyphs) const
	{
		if (!is_valid() || text.empty()) return;
		float cx = x;
		for (unsigned char ch : text)
		{
			auto it = glyphs.find(static_cast<std::uint16_t>(ch));
			if (it == glyphs.end())
			{
				if (missing_glyphs) ++(*missing_glyphs);
				continue;
			}
			const IwGlyph& gl = it->second;
			if (gl.pixel_width > 0 && gl.pixel_height > 0)
			{
				gfx::UiDrawCmd cmd{};
				cmd.x = cx + static_cast<float>(gl.x0) * scale;
				cmd.y = y + static_cast<float>(gl.y0) * scale;
				cmd.w = static_cast<float>(gl.pixel_width) * scale;
				cmd.h = static_cast<float>(gl.pixel_height) * scale;
				cmd.u0 = gl.s0;
				cmd.v0 = gl.t0;
				cmd.u1 = gl.s1;
				cmd.v1 = gl.t1;
				cmd.r = r; cmd.g = g; cmd.b = b; cmd.a = a;
				cmd.blend = gfx::UiBlendMode::Alpha;
				cmd.texture = atlas.get();
				out.push_back(cmd);
			}
			cx += static_cast<float>(gl.dx) * scale;
		}
	}

	void ScaleformGfxMovie::emit_export(const std::string& export_name, int fb_w, int fb_h,
		assets::AssetManager& assets, std::vector<gfx::UiDrawCmd>& out,
		bool center_sprite_on_screen,
		std::vector<std::string>* resolved_origins,
		std::vector<std::string>* unfinished_notes)
	{
		auto it = exports_by_name.find(export_name);
		if (it == exports_by_name.end())
		{
			if (unfinished_notes)
				unfinished_notes->push_back("Scaleform export '" + export_name + "' not found in " + rawfile_name);
			return;
		}

		struct RawLayer
		{
			float x, y, w, h;
			float r, g, b, a;
			gfx::UiBlendMode blend;
			const assets::DecodedTexture* tex;
		};
		std::vector<RawLayer> layers;

		auto character_id = [&](const ScaleformPlacement& pl) -> std::uint16_t {
			if (pl.has_char) return pl.char_id;
			const auto symbol = symbol_classes.find(pl.class_name);
			return symbol == symbol_classes.end() ? 0 : symbol->second;
		};

		auto walk = [&](auto&& self, std::uint16_t sprite_id,
			float ptx, float pty, float psx, float psy,
			float pr, float pg, float pb, float pa,
			gfx::UiBlendMode pblend, int depth_guard) -> void
		{
			if (depth_guard > 16) return;
			auto sit = sprites.find(sprite_id);
			if (sit == sprites.end()) return;

			for (const auto& pl : sit->second.frame0_places)
			{
				if (pl.is_clip_mask) continue;
				const float tx = ptx + pl.tx * psx;
				const float ty = pty + pl.ty * psy;
				const float sx = psx * pl.sx;
				const float sy = psy * pl.sy;
				const float r = pr * pl.mul_r;
				const float g = pg * pl.mul_g;
				const float b = pb * pl.mul_b;
				const float a = pa * pl.mul_a;
				const gfx::UiBlendMode blend = (pl.blend_mode == 8) ? gfx::UiBlendMode::Additive : pblend;

				const auto child = character_id(pl);
				if (!child)
				{
					if (!pl.class_name.empty() && unfinished_notes)
					{
						unfinished_notes->push_back("ActionScript class instance '" + pl.instance_name +
							"' (class '" + pl.class_name + "') requires AS3 runtime (NOT_IMPLEMENTED)");
					}
					continue;
				}

				auto img_it = external_images.find(child);
				if (img_it != external_images.end())
				{
					auto& ext = img_it->second;
					if (!ext.texture)
					{
						ext.texture = assets.get_streamed_texture(ext.export_name, 2048);
						codol::render::iwd_archive_set::hit h{};
						if (assets.image_archives().find(ext.export_name, h) && h.archive)
						{
							ext.source_origin = source_zone + ".ff:" + rawfile_name +
								" (DefineExternalImage2 id=" + std::to_string(ext.char_id) + ") -> " +
								h.archive->file_name() + ":images/" + ext.export_name + ".iwi";
						}
						else
						{
							ext.source_origin = source_zone + ".ff:" + rawfile_name +
								" (DefineExternalImage2 id=" + std::to_string(ext.char_id) + ") -> [MISSING IWI: " + ext.export_name + "]";
						}
					}
					if (ext.texture && ext.texture->is_valid())
					{
						const float lw = static_cast<float>(ext.width) * sx;
						const float lh = static_cast<float>(ext.height) * sy;
						layers.push_back({ tx, ty, lw, lh, r, g, b, a, blend, ext.texture.get() });
						if (resolved_origins)
							resolved_origins->push_back(ext.source_origin);
					}
					else if (unfinished_notes)
					{
						unfinished_notes->push_back("Missing external Scaleform image '" + ext.export_name +
							"' (DefineExternalImage2 id=" + std::to_string(ext.char_id) + ")");
					}
					continue;
				}

				if (sprites.find(child) != sprites.end())
				{
					self(self, child, tx, ty, sx, sy, r, g, b, a, blend, depth_guard + 1);
				}
			}
		};

		bool placed = false;
		if (!center_sprite_on_screen)
		{
			for (const auto& [depth, instance] : root_instances)
			{
				if (instance.is_clip_mask || character_id(instance) != it->second) continue;
				placed = true;
				walk(walk, it->second, instance.tx, instance.ty, instance.sx, instance.sy,
					instance.mul_r, instance.mul_g, instance.mul_b, instance.mul_a,
					instance.blend_mode == 8 ? gfx::UiBlendMode::Additive : gfx::UiBlendMode::Alpha, 0);
			}
			if (!placed && unfinished_notes)
				unfinished_notes->push_back("No direct first-frame root placement for " + export_name);
		}
		if (!placed)
			walk(walk, it->second, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, gfx::UiBlendMode::Alpha, 0);
		if (layers.empty()) return;

		const float W = static_cast<float>(fb_w);
		const float H = static_cast<float>(fb_h);
		const float stage_scale = std::min(W / stage_width, H / stage_height);

		if (center_sprite_on_screen)
		{
			float min_x = std::numeric_limits<float>::max();
			float min_y = std::numeric_limits<float>::max();
			float max_x = std::numeric_limits<float>::lowest();
			float max_y = std::numeric_limits<float>::lowest();
			for (const auto& l : layers)
			{
				min_x = std::min(min_x, l.x);
				min_y = std::min(min_y, l.y);
				max_x = std::max(max_x, l.x + l.w);
				max_y = std::max(max_y, l.y + l.h);
			}
			const float bw = std::max(1.0f, max_x - min_x);
			const float bh = std::max(1.0f, max_y - min_y);
			const float sc = std::min((W * 0.60f) / bw, (H * 0.45f) / bh);
			const float ox = (W - bw * sc) * 0.5f - min_x * sc;
			const float oy = (H - bh * sc) * 0.5f - min_y * sc;
			for (const auto& l : layers)
			{
				gfx::UiDrawCmd cmd{};
				cmd.x = ox + l.x * sc;
				cmd.y = oy + l.y * sc;
				cmd.w = l.w * sc;
				cmd.h = l.h * sc;
				cmd.r = l.r; cmd.g = l.g; cmd.b = l.b; cmd.a = l.a;
				cmd.blend = l.blend;
				cmd.texture = l.tex;
				out.push_back(cmd);
			}
		}
		else
		{
			// Exact 1280x960 Scaleform stage mapping centered with aspect ratio preserved
			const float off_x = (W - stage_width * stage_scale) * 0.5f;
			const float off_y = (H - stage_height * stage_scale) * 0.5f;
			for (const auto& l : layers)
			{
				gfx::UiDrawCmd cmd{};
				cmd.x = off_x + (l.x - stage_x) * stage_scale;
				cmd.y = off_y + (l.y - stage_y) * stage_scale;
				cmd.w = l.w * stage_scale;
				cmd.h = l.h * stage_scale;
				cmd.r = l.r; cmd.g = l.g; cmd.b = l.b; cmd.a = l.a;
				cmd.blend = l.blend;
				cmd.texture = l.tex;
				out.push_back(cmd);
			}
		}
	}

	bool AuthenticUiSystem::init(assets::AssetManager& assets)
	{
		unfinished_log_.clear();
		const std::string& root = assets.report().game_root;
		if (root.empty())
		{
			platform::log_warn("[UI][MISSING] Game directory not mounted — cannot load original UI/startup assets.");
			return false;
		}

		// 1. Original root splash.bmp (Sys_ShowSplashWindow @ 0x00632908) & logo.bmp (Sys_CreateConsole @ 0x00632f25)
		std::string err;
		const std::string splash_path = join_path(root, "splash.bmp");
		splash_bmp_ = load_bmp_texture(splash_path, "splash.bmp", &err);
		if (splash_bmp_)
		{
			platform::log_info("[UI][OK] Loaded original startup splash: splash.bmp (%dx%d, Sys_ShowSplashWindow @ 0x00632908)",
				splash_bmp_->width, splash_bmp_->height);
		}
		else
		{
			const std::string msg = "[UI][MISSING] splash.bmp not found in '" + root + "' (" + err + ")";
			unfinished_log_.push_back(msg);
			platform::log_warn("%s", msg.c_str());
		}

		const std::string logo_path = join_path(root, "logo.bmp");
		logo_bmp_ = load_bmp_texture(logo_path, "logo.bmp", &err);
		if (logo_bmp_)
		{
			platform::log_info("[UI][OK] Loaded original console banner: logo.bmp (%dx%d, Sys_CreateConsole @ 0x00632f25)",
				logo_bmp_->width, logo_bmp_->height);
		}

		// 2. Original Bink 2 intro cinematics (autocinematic PUB_logo @ 0x00598e0d, CODO_logo @ 0x00598e31)
		pub_logo_bk2_ = inspect_bink2_file(join_path(root, "main/video/PUB_logo.bk2"), "main/video/PUB_logo.bk2");
		codo_logo_bk2_ = inspect_bink2_file(join_path(root, "main/video/CODO_logo.bk2"), "main/video/CODO_logo.bk2");
		auto log_bink = [&](const Bink2Info& bi) {
			if (bi.found && bi.valid_header)
			{
				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"[UI][UNFINISHED] Intro cinematic '%s' (%s %ux%u, %u frames) present, but Bink 2 (KB2i) decoder is NOT_IMPLEMENTED.",
					bi.rel_path.c_str(), bi.magic, bi.width, bi.height, bi.frame_count);
				unfinished_log_.emplace_back(buf);
				platform::log_warn("%s", buf);
			}
			else
			{
				const std::string msg = "[UI][MISSING] Intro cinematic '" + bi.rel_path + "' not found.";
				unfinished_log_.push_back(msg);
				platform::log_warn("%s", msg.c_str());
			}
		};
		log_bink(pub_logo_bk2_);
		log_bink(codo_logo_bk2_);

		// 3. Original IW fonts & menus (code_post_gfx_mp.ff, patch_code_post_gfx_mp.ff)
		load_iw_fonts_and_menus(assets, "code_post_gfx_mp");
		load_iw_fonts_and_menus(assets, "patch_code_post_gfx_mp");
		load_iw_fonts_and_menus(assets, "common_postgfx_mp");

		// 4. Localized strings (localized_code_post_gfx_mp.ff)
		if (auto loc_zone = assets.load_zone("localized_code_post_gfx_mp"))
		{
			for (const auto& a : loc_zone->all_assets())
			{
				if (a.type != codol::c1::ASSET_TYPE_LOCALIZE_ENTRY) continue;
				codol::zone::LocalizeView lv;
				if (codol::zone::read_localize(*loc_zone, a.header, lv) && !lv.name.empty())
					localized_.emplace(std::string(lv.name), std::string(lv.value));
			}
			platform::log_info("[UI][OK] Loaded %zu localized entries from localized_code_post_gfx_mp.ff (GBK/GB18030)",
				localized_.size());
		}

		// 5. Original Scaleform GFx movies (common_ui_mp.ff -> patch_ui_mp.ff -> ui_mp.ff precedence)
		load_scaleform_movie(assets, "scaleform/default/codologin.gfx", { "common_ui_mp", "ui_mp" }, codologin_gfx_);
		load_scaleform_movie(assets, "scaleform/default/codo.gfx", { "common_ui_mp", "patch_ui_mp", "ui_mp" }, codo_gfx_);

		// Explicitly note unfinished CJK Scaleform font rendering
		unfinished_log_.push_back("[UI][UNFINISHED] Scaleform CJK vector font atlas (scaleform/fonts_ingame.gfx in patch_code_post_gfx_mp.ff) is NOT_IMPLEMENTED.");
		unfinished_log_.push_back("[UI][UNFINISHED] Interactive Scaleform AS3 menu runtime (DoABC tag 82 in codologin.gfx / codo.gfx / codohud.gfx) is NOT_IMPLEMENTED.");
		platform::log_warn("[UI][UNFINISHED] Interactive Scaleform AS3 VM (DoABC) & CJK vector font renderer (fonts_ingame.gfx) are NOT_IMPLEMENTED.");

		return splash_bmp_ != nullptr || !menus_.empty() || codologin_gfx_.loaded() || codo_gfx_.loaded();
	}

	bool AuthenticUiSystem::load_iw_fonts_and_menus(assets::AssetManager& assets, const std::string& zone_name)
	{
		std::string err;
		auto z = assets.load_zone(zone_name, &err);
		if (!z)
		{
			platform::log_warn("[UI][MISSING] FastFile '%s.ff' could not be loaded: %s", zone_name.c_str(), err.c_str());
			return false;
		}
		if (zone_name == "patch_code_post_gfx_mp")
			patch_code_post_gfx_zone_ = z;

		int loaded_fonts = 0;
		int loaded_menus = 0;
		for (const auto& a : z->all_assets())
		{
			if (a.type == codol::c1::ASSET_TYPE_FONT)
			{
				FontHeaderC1 fh{};
				if (!z->get(a.header, fh)) continue;
				IwFont font;
				font.name = std::string(z->c_string(fh.fontName));
				font.source_zone = zone_name;
				font.pixel_height = fh.pixelHeight;
				font.material_name = std::string(z->pointee_name(codol::c1::ASSET_TYPE_MATERIAL, fh.material));
				if (fh.material)
				{
					auto mat = assets.resolve_material(*z, fh.material, 2048);
					if (mat && mat->color_texture && mat->color_texture->is_valid())
						font.atlas = mat->color_texture;
				}
				if (!font.atlas && !font.material_name.empty())
				{
					// External zone material reference (e.g. "fonts/gamefonts_pc" -> localized_chinese_iw_image_00.iwd:images/gamefonts_pc.iwi)
					std::string stem = font.material_name;
					const std::size_t slash = stem.find_last_of('/');
					if (slash != std::string::npos) stem = stem.substr(slash + 1);
					const std::size_t glow_pos = stem.rfind("_glow");
					if (glow_pos != std::string::npos && glow_pos + 5 == stem.size())
						stem.resize(glow_pos);
					font.atlas = assets.get_streamed_texture(stem, 2048);
				}
				if (fh.glyphCount > 0 && fh.glyphCount < 65536 && fh.glyphs)
				{
					auto raw_glyphs = z->get_array<GlyphC1>(fh.glyphs, static_cast<std::size_t>(fh.glyphCount));
					for (const auto& rg : raw_glyphs)
					{
						IwGlyph g{};
						g.letter = rg.letter;
						g.x0 = rg.x0;
						g.y0 = rg.y0;
						g.dx = rg.dx;
						g.pixel_width = rg.pixelWidth;
						g.pixel_height = rg.pixelHeight;
						g.s0 = rg.s0; g.t0 = rg.t0; g.s1 = rg.s1; g.t1 = rg.t1;
						font.glyphs.emplace(g.letter, g);
					}
				}
				if (!font.name.empty() && !font.glyphs.empty())
				{
					fonts_[font.name] = std::move(font);
					++loaded_fonts;
				}
			}
			else if (a.type == codol::c1::ASSET_TYPE_MENU)
			{
				WindowDefC1 mw{};
				if (!z->get(a.header, mw)) continue;
				IwMenuDef menu;
				menu.name = std::string(z->c_string(mw.name));
				menu.source_zone = zone_name;
				menu.x = mw.rect.x; menu.y = mw.rect.y;
				menu.w = mw.rect.w; menu.h = mw.rect.h;

				const std::int32_t item_count = z->get<std::int32_t>(a.header + 0xB0);
				const codol::c1::zptr items_ptr = z->get<codol::c1::zptr>(a.header + 0x11C);
				if (item_count > 0 && item_count < 512 && items_ptr)
				{
					auto ptrs = z->get_array<codol::c1::zptr>(items_ptr, static_cast<std::size_t>(item_count));
					for (codol::c1::zptr ip : ptrs)
					{
						WindowDefC1 iw{};
						if (!z->get(ip, iw)) continue;
						IwMenuItem item;
						item.name = std::string(z->c_string(iw.name));
						item.x = iw.rect.x; item.y = iw.rect.y;
						item.w = iw.rect.w; item.h = iw.rect.h;
						item.horiz_align = iw.rect.horizAlign;
						item.vert_align = iw.rect.vertAlign;
						item.style = iw.style;
						item.owner_draw = iw.ownerDraw;
						std::memcpy(item.fore_color, iw.foreColor, sizeof(item.fore_color));
						std::memcpy(item.back_color, iw.backColor, sizeof(item.back_color));
						item.background_material = std::string(z->pointee_name(codol::c1::ASSET_TYPE_MATERIAL, iw.background));
						const codol::c1::zptr text_ptr = z->get<codol::c1::zptr>(ip + 0xE4);
						if (text_ptr) item.text = std::string(z->c_string(text_ptr));

						if (iw.background && !item.background_material.empty() &&
							item.background_material != "white" && item.background_material != "black" &&
							item.background_material[0] != '$')
						{
							auto mat = assets.resolve_material(*z, iw.background, 2048);
							if (mat && mat->color_texture && mat->color_texture->is_valid())
							{
								item.resolved_texture = mat->color_texture;
								codol::render::iwd_archive_set::hit h{};
								if (assets.image_archives().find(mat->color_map_name, h) && h.archive)
								{
									item.texture_origin = zone_name + ".ff (mat:" + item.background_material + ") -> " +
										h.archive->file_name() + ":images/" + mat->color_map_name + ".iwi";
								}
								else
								{
									item.texture_origin = zone_name + ".ff (mat:" + item.background_material + ")";
								}
							}
						}
						menu.items.push_back(std::move(item));
					}
				}
				if (!menu.name.empty())
				{
					menus_[menu.name] = std::move(menu);
					++loaded_menus;
				}
			}
		}

		platform::log_info("[UI][OK] Loaded %d IW fonts and %d IW menus from %s.ff",
			loaded_fonts, loaded_menus, zone_name.c_str());
		return (loaded_fonts > 0 || loaded_menus > 0);
	}

	bool AuthenticUiSystem::load_scaleform_movie(assets::AssetManager& assets, const std::string& rawfile_name,
		const std::vector<std::string>& candidate_zones, ScaleformGfxMovie& out)
	{
		for (const auto& zn : candidate_zones)
		{
			auto z = assets.load_zone(zn);
			if (!z) continue;
			const codol::zone::zaddr h = z->find_asset(codol::c1::ASSET_TYPE_RAWFILE, rawfile_name);
			if (!h) continue;
			codol::zone::RawFileView rv;
			if (!codol::zone::read_rawfile(*z, h, rv)) continue;
			std::vector<std::uint8_t> content;
			auto st = codol::zone::rawfile_contents(rv, content);
			if (!st.ok || content.empty()) continue;

			out = ScaleformGfxMovie{};
			out.rawfile_name = rawfile_name;
			out.source_zone = zn;
			if (parse_scaleform_bytes(content, out))
			{
				platform::log_info("[UI][OK] Parsed Scaleform '%s' from %s.ff (GFX v%u, %.0fx%.0f, %zu ext images, %zu sprites, %zu exports; UNFINISHED tags: DoABC=%u, DefineShape=%u, EditText=%u)",
					rawfile_name.c_str(), zn.c_str(), out.swf_version,
					out.stage_width, out.stage_height,
					out.external_images.size(), out.sprites.size(), out.exports_by_name.size(),
					out.do_abc_tag_count, out.define_shape_tag_count, out.define_edit_text_tag_count);
				return true;
			}
		}
		const std::string msg = "[UI][MISSING] Scaleform movie '" + rawfile_name + "' not found in candidate UI FastFiles.";
		unfinished_log_.push_back(msg);
		platform::log_warn("%s", msg.c_str());
		return false;
	}

	bool AuthenticUiSystem::parse_scaleform_bytes(const std::vector<std::uint8_t>& bytes, ScaleformGfxMovie& out)
	{
		if (bytes.size() < 16) return false;
		const bool is_uncompressed =
			(bytes[0] == 'G' && bytes[1] == 'F' && bytes[2] == 'X') ||
			(bytes[0] == 'F' && bytes[1] == 'W' && bytes[2] == 'S');
		if (!is_uncompressed)
		{
			platform::log_warn("[UI][UNFINISHED] Scaleform '%s' uses compressed signature '%c%c%c' (not yet supported by lightweight parser)",
				out.rawfile_name.c_str(), bytes[0], bytes[1], bytes[2]);
			return false;
		}
		out.swf_version = bytes[3];

		const std::uint8_t* body = bytes.data() + 8;
		const std::size_t body_len = bytes.size() - 8;
		BitReader br{ body, body_len, 0 };
		const std::uint32_t nbits = br.u(5);
		const std::int32_t xmin = br.s(nbits);
		const std::int32_t xmax = br.s(nbits);
		const std::int32_t ymin = br.s(nbits);
		const std::int32_t ymax = br.s(nbits);
		br.align_byte();
		std::size_t off = br.byte_pos() + 4; // skip fps (2B) + frameCount (2B)
		out.stage_x = static_cast<float>(xmin) / 20.0f;
		out.stage_y = static_cast<float>(ymin) / 20.0f;
		bool first_root_frame = true;
		out.stage_width = static_cast<float>(xmax - xmin) / 20.0f;
		out.stage_height = static_cast<float>(ymax - ymin) / 20.0f;

		while (off + 2 <= body_len)
		{
			const std::uint16_t tc = rd_u16(body + off);
			off += 2;
			const std::uint16_t tag = tc >> 6;
			std::uint32_t length = tc & 0x3Fu;
			if (length == 0x3Fu)
			{
				if (off + 4 > body_len) break;
				length = rd_u32(body + off);
				off += 4;
			}
			if (off + length > body_len) break;
			const std::uint8_t* pl = body + off;
			off += length;

			if (tag == 0) break;
			else if (tag == 1) first_root_frame = false;
			else if (first_root_frame && (tag == 26 || tag == 70))
			{
				ScaleformPlacement placement{};
				if (!parse_swf_place_object(tag, pl, length, bytes[0] == 'G', placement)) continue;
				if ((pl[0] & 1) && out.root_instances.count(placement.depth))
				{
					auto& current = out.root_instances.at(placement.depth);
					if (placement.has_char) { current.has_char = true; current.char_id = placement.char_id; }
					if (!placement.class_name.empty()) current.class_name = placement.class_name;
					if (placement.has_matrix)
					{
						current.tx = placement.tx; current.ty = placement.ty;
						current.sx = placement.sx; current.sy = placement.sy;
					}
					if (placement.has_cxform)
					{
						current.mul_r = placement.mul_r; current.mul_g = placement.mul_g;
						current.mul_b = placement.mul_b; current.mul_a = placement.mul_a;
					}
					if (placement.is_clip_mask) current.is_clip_mask = true;
					if (placement.blend_mode) current.blend_mode = placement.blend_mode;
				}
				else out.root_instances[placement.depth] = std::move(placement);
			}
			else if (first_root_frame && tag == 28 && length >= 2)
				out.root_instances.erase(rd_u16(pl));
			else if (first_root_frame && tag == 5 && length >= 4)
				out.root_instances.erase(rd_u16(pl + 2));
			else if (tag == 82) ++out.do_abc_tag_count;
			else if (tag == 2 || tag == 22 || tag == 32 || tag == 83) ++out.define_shape_tag_count;
			else if (tag == 37) ++out.define_edit_text_tag_count;
			else if (tag == 1001 || tag == 1009)
			{
				const std::size_t hdr_sz = (tag == 1001) ? 8 : 10;
				if (length >= hdr_sz + 2)
				{
					ScaleformExternalImage ext{};
					if (tag == 1001)
					{
						ext.char_id = rd_u16(pl + 0);
						ext.width = rd_u16(pl + 4);
						ext.height = rd_u16(pl + 6);
					}
					else
					{
						ext.char_id = rd_u32(pl + 0);
						ext.width = rd_u16(pl + 6);
						ext.height = rd_u16(pl + 8);
					}
					std::size_t p = hdr_sz;
					const std::uint8_t l1 = pl[p++];
					if (p + l1 <= length)
					{
						ext.export_name.assign(reinterpret_cast<const char*>(pl + p), l1);
						p += l1;
					}
					if (p < length)
					{
						const std::uint8_t l2 = pl[p++];
						if (p + l2 <= length)
							ext.file_name.assign(reinterpret_cast<const char*>(pl + p), l2);
					}
					out.external_images.emplace(ext.char_id, std::move(ext));
				}
			}
			else if (tag == 56 || tag == 76)
			{
				if (length >= 2)
				{
					const std::uint16_t cnt = rd_u16(pl);
					std::size_t p = 2;
					for (std::uint16_t i = 0; i < cnt && p + 2 <= length; ++i)
					{
						const std::uint16_t cid = rd_u16(pl + p);
						p += 2;
						const std::size_t start = p;
						while (p < length && pl[p] != 0) ++p;
						if (p >= length) break;
						std::string name(reinterpret_cast<const char*>(pl + start), p - start);
						++p;
						out.exports_by_name[name] = cid;
						if (tag == 76 && cid) out.symbol_classes[name] = cid;
					}
				}
			}
			else if (tag == 39 && length >= 4)
			{
				ScaleformSprite sp{};
				sp.id = rd_u16(pl + 0);
				sp.frame_count = rd_u16(pl + 2);
				std::size_t soff = 4;
				while (soff + 2 <= length)
				{
					const std::uint16_t stc = rd_u16(pl + soff);
					soff += 2;
					const std::uint16_t stag = stc >> 6;
					std::uint32_t slen = stc & 0x3Fu;
					if (slen == 0x3Fu)
					{
						if (soff + 4 > length) break;
						slen = rd_u32(pl + soff);
						soff += 4;
					}
					if (soff + slen > length) break;
					const std::uint8_t* spl = pl + soff;
					soff += slen;

					if (stag == 0 || stag == 1) break; // stop after frame 0 ShowFrame
					if (stag == 26 || stag == 70)
					{
						ScaleformPlacement place{};
						if (parse_swf_place_object(stag, spl, slen, bytes[0] == 'G', place))
							sp.frame0_places.push_back(std::move(place));
					}
				}
				std::stable_sort(sp.frame0_places.begin(), sp.frame0_places.end(),
					[](const ScaleformPlacement& a, const ScaleformPlacement& b) { return a.depth < b.depth; });
				out.sprites.emplace(sp.id, std::move(sp));
			}
		}
		return out.stage_width > 0.0f && out.stage_height > 0.0f;
	}

	const IwMenuDef* AuthenticUiSystem::find_menu(const std::string& name) const
	{
		auto it = menus_.find(name);
		return it != menus_.end() ? &it->second : nullptr;
	}

	const IwFont* AuthenticUiSystem::find_font(const std::string& name) const
	{
		auto it = fonts_.find(name);
		return it != fonts_.end() ? &it->second : nullptr;
	}

	std::string AuthenticUiSystem::localize(const std::string& key) const
	{
		std::string k = key;
		if (!k.empty() && k[0] == '@') k = k.substr(1);
		auto it = localized_.find(k);
		return it != localized_.end() ? it->second : std::string();
	}

	void AuthenticUiSystem::emit_iw_menu(const IwMenuDef& menu, int fb_w, int fb_h, float load_progress,
		assets::AssetManager& assets, const std::string& active_map, std::vector<gfx::UiDrawCmd>& out)
	{
		const float prog = std::clamp(load_progress, 0.0f, 1.0f);
		for (const auto& item : menu.items)
		{
			float x = 0, y = 0, w = 0, h = 0;
			apply_iw_rect(item.x, item.y, item.w, item.h, item.horiz_align, item.vert_align, fb_w, fb_h, x, y, w, h);

			// Handle negative height/width (flipped rect in original menuDef)
			float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
			if (w < 0.0f) { x += w; w = -w; std::swap(u0, u1); }
			if (h < 0.0f) { y += h; h = -h; std::swap(v0, v1); }

			// style 1 = WINDOW_STYLE_FILLED
			if (item.style == 1 && item.back_color[3] > 0.001f && w > 0.0f && h > 0.0f)
			{
				gfx::UiDrawCmd cmd{};
				cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
				cmd.r = item.back_color[0];
				cmd.g = item.back_color[1];
				cmd.b = item.back_color[2];
				cmd.a = item.back_color[3];
				cmd.blend = gfx::UiBlendMode::Alpha;
				cmd.texture = nullptr;
				out.push_back(cmd);
			}
			// style 3 = WINDOW_STYLE_SHADER
			else if (item.style == 3)
			{
				const assets::DecodedTexture* tex = item.resolved_texture.get();
				if (!tex && item.background_material == "$levelbriefing" && !active_map.empty())
				{
					auto map_load = assets.get_streamed_texture("loadscreen_" + active_map, 2048);
					if (map_load && map_load->is_valid())
					{
						// Fullscreen level briefing backdrop
						gfx::UiDrawCmd cmd{};
						cmd.x = 0.0f; cmd.y = 0.0f;
						cmd.w = static_cast<float>(fb_w);
						cmd.h = static_cast<float>(fb_h);
						cmd.r = item.fore_color[0]; cmd.g = item.fore_color[1];
						cmd.b = item.fore_color[2]; cmd.a = item.fore_color[3];
						cmd.blend = gfx::UiBlendMode::Opaque;
						cmd.texture = map_load.get();
						out.push_back(cmd);
						continue;
					}
				}
				if (tex && w > 0.0f && h > 0.0f)
				{
					gfx::UiDrawCmd cmd{};
					cmd.x = x; cmd.y = y; cmd.w = w; cmd.h = h;
					cmd.u0 = u0; cmd.v0 = v0; cmd.u1 = u1; cmd.v1 = v1;
					cmd.r = item.fore_color[0]; cmd.g = item.fore_color[1];
					cmd.b = item.fore_color[2]; cmd.a = item.fore_color[3];
					cmd.blend = gfx::UiBlendMode::Alpha;
					cmd.texture = tex;
					out.push_back(cmd);
				}
			}
			// style 6 = WINDOW_STYLE_LOADBAR (progress bar)
			else if (item.style == 6 && item.name == "loadbar" && w > 0.0f && h > 0.0f)
			{
				gfx::UiDrawCmd cmd{};
				cmd.x = x; cmd.y = y; cmd.w = w * prog; cmd.h = h;
				cmd.r = item.fore_color[0];
				cmd.g = item.fore_color[1];
				cmd.b = item.fore_color[2];
				cmd.a = item.fore_color[3];
				cmd.blend = gfx::UiBlendMode::Alpha;
				cmd.texture = nullptr;
				out.push_back(cmd);
			}
		}
	}

	void AuthenticUiSystem::build_stage_cmds(StartupStage stage, int fb_w, int fb_h, float load_progress,
		assets::AssetManager& assets, std::vector<gfx::UiDrawCmd>& out, const std::string& active_map)
	{
		out.clear();
		if (fb_w <= 0 || fb_h <= 0) return;

		if (stage == StartupStage::Splash)
		{
			if (!splash_bmp_ || !splash_bmp_->is_valid()) return;
			// Original Sys_ShowSplashWindow centers splash.bmp (800x510) preserving exact 800:510 aspect ratio
			const float W = static_cast<float>(fb_w);
			const float H = static_cast<float>(fb_h);
			const float sw = static_cast<float>(splash_bmp_->width);
			const float sh = static_cast<float>(splash_bmp_->height);
			const float sc = std::min((W * 0.85f) / sw, (H * 0.85f) / sh);
			const float dw = sw * sc;
			const float dh = sh * sc;
			gfx::UiDrawCmd cmd{};
			cmd.x = (W - dw) * 0.5f;
			cmd.y = (H - dh) * 0.5f;
			cmd.w = dw;
			cmd.h = dh;
			cmd.blend = gfx::UiBlendMode::Opaque;
			cmd.texture = splash_bmp_.get();
			out.push_back(cmd);
		}
		else if (stage == StartupStage::LoadingMenu)
		{
			if (const IwMenuDef* m = find_menu("load"))
				emit_iw_menu(*m, fb_w, fb_h, load_progress, assets, active_map, out);
		}
		else if (stage == StartupStage::MapLoading)
		{
			if (const IwMenuDef* m = find_menu("connect"))
				emit_iw_menu(*m, fb_w, fb_h, load_progress, assets, active_map, out);
		}
		else if (stage == StartupStage::LoginTitle)
		{
			std::vector<std::string> origins;
			std::vector<std::string> notes;
			codologin_gfx_.emit_export("loginView", fb_w, fb_h, assets, out, false,
				logged_login_unfinished_ ? nullptr : &origins, logged_login_unfinished_ ? nullptr : &notes);
			if (!logged_login_unfinished_)
			{
				logged_login_unfinished_ = true;
				for (const auto& cmd : out)
					platform::log_info("[UI][LAYOUT] loginView %s screen=(%.1f,%.1f %.1fx%.1f) framebuffer=%dx%d",
						cmd.texture ? cmd.texture->name.c_str() : "(solid)", cmd.x, cmd.y, cmd.w, cmd.h, fb_w, fb_h);
				for (const auto& o : origins)
					platform::log_info("[UI][SOURCE] loginView layer: %s", o.c_str());
				for (const auto& n : notes)
					platform::log_warn("[UI][UNFINISHED] loginView: %s", n.c_str());
			}
		}
		else if (stage == StartupStage::MainMenuLogo)
		{
			std::vector<std::string> origins;
			std::vector<std::string> notes;
			codo_gfx_.emit_export("logoView", fb_w, fb_h, assets, out, true, &origins, &notes);
			if (!logged_logo_unfinished_)
			{
				logged_logo_unfinished_ = true;
				for (const auto& o : origins)
					platform::log_info("[UI][SOURCE] logoView layer: %s", o.c_str());
				for (const auto& n : notes)
					platform::log_warn("[UI][UNFINISHED] logoView: %s", n.c_str());
			}
		}
		else if (stage == StartupStage::CyborgsLogo)
		{
			std::vector<std::string> origins;
			std::vector<std::string> notes;
			codo_gfx_.emit_export("logoCyborgsView", fb_w, fb_h, assets, out, true, &origins, &notes);
		}
	}

	LobbyCameraSpec AuthenticUiSystem::resolve_lobby_camera(const codol::zone::Zone& ui_viewer_zone,
		const std::string& preset_name) const
	{
		LobbyCameraSpec spec{};
		spec.preset_name = preset_name;
		spec.camera_targetname = "player_camera_center";
		spec.look_targetname = "player_pos_center";

		// 1. Look up preset in mp/camerapostable.csv from patch_code_post_gfx_mp.ff
		if (patch_code_post_gfx_zone_)
		{
			const codol::zone::zaddr th = patch_code_post_gfx_zone_->find_asset(
				codol::c1::ASSET_TYPE_STRINGTABLE, "mp/camerapostable.csv");
			codol::zone::StringTableView st;
			if (th && codol::zone::read_stringtable(*patch_code_post_gfx_zone_, th, st))
			{
				for (int r = 0; r < st.raw.rowCount; ++r)
				{
					if (st.cell(r, 0) == preset_name)
					{
						spec.look_targetname = std::string(st.cell(r, 2));
						spec.camera_targetname = std::string(st.cell(r, 3));
						break;
					}
				}
			}
		}

		// 2. Parse script_origin entities from ui_viewer_mp.ff MapEnts
		const codol::zone::zaddr mh = codol::zone::find_map_ents(ui_viewer_zone);
		codol::zone::MapEntsView mv;
		if (!mh || !codol::zone::read_map_ents(ui_viewer_zone, mh, mv))
			return spec;

		world::EntityWorld ew;
		ew.parse_entity_string(mv.entityString);

		bool found_cam = false;
		math::Vec3 cam_origin{};
		float cam_pitch = 0.0f, cam_yaw = 0.0f;
		for (const auto& e : ew.entities())
		{
			auto tn = e.kv.find("targetname");
			if (tn == e.kv.end()) continue;
			if (tn->second == spec.camera_targetname)
			{
				auto org = e.kv.find("origin");
				if (org != e.kv.end())
				{
					std::sscanf(org->second.c_str(), "%f %f %f", &cam_origin.x, &cam_origin.y, &cam_origin.z);
					found_cam = true;
				}
				auto ang = e.kv.find("angles");
				if (ang != e.kv.end())
				{
					float roll = 0.0f;
					std::sscanf(ang->second.c_str(), "%f %f %f", &cam_pitch, &cam_yaw, &roll);
				}
				break;
			}
		}
		if (found_cam)
		{
			spec.valid = true;
			spec.eye = cam_origin;
			spec.yaw_deg = cam_yaw;
			spec.pitch_deg = -cam_pitch; // CoD pitch convention
			spec.fov_deg = 55.0f;
		}
		return spec;
	}
}
