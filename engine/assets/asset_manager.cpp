#include "asset_manager.hpp"
#include "../platform/platform.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace dragon::assets
{
	namespace
	{
		std::string lower_ascii(std::string s)
		{
			for (char& c : s)
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
			return s;
		}

		std::string trim_comma(std::string_view sv)
		{
			while (!sv.empty() && sv.front() == ',') sv.remove_prefix(1);
			return std::string(sv);
		}

		bool starts_with(const std::string& s, const char* prefix)
		{
			const std::size_t n = std::strlen(prefix);
			return s.size() >= n && s.compare(0, n, prefix) == 0;
		}

		bool contains_ci(const std::string& hay, const char* needle)
		{
			return lower_ascii(hay).find(needle) != std::string::npos;
		}
	}

	MountReport AssetManager::mount(const std::string& game_root)
	{
		clear_caches();
		mounted_ = false;
		report_ = {};
		report_.game_root = game_root;
		fastfile_paths_.clear();

		if (game_root.empty() || !platform::dir_exists(game_root))
		{
			report_.error = "Game directory does not exist: " + game_root;
			return report_;
		}

		// 1. Locate retail client executable for runtime key + wavelet table extraction
		const std::vector<std::string> exe_candidates = {
			game_root + "/codoMP_client_shipRetail.exe",
			game_root + "/C1.exe",
			game_root + "/codol.exe"
		};
		for (const auto& cand : exe_candidates)
		{
			if (platform::file_exists(cand))
			{
				report_.exe_path = cand;
				break;
			}
		}
		if (report_.exe_path.empty())
		{
			report_.error = "Could not find codoMP_client_shipRetail.exe (or C1.exe) in " + game_root;
			return report_;
		}

		// 2. Extract FastFile RSA-2048 private key from user's exe
		if (const auto st = ff_key_.load_from_exe(report_.exe_path); !st)
		{
			report_.error = "FastFile RSA key load failed: " + st.message;
			return report_;
		}
		report_.has_fastfile_key = true;

		// 3. Derive 24-byte IWD AES-192 key from user's exe
		{
			std::string err;
			if (!codol::render::derive_iwd_key(report_.exe_path, iwd_key_, &err))
			{
				report_.warnings.push_back("IWD key derivation failed: " + err);
			}
			else
			{
				report_.has_iwd_key = true;
			}
		}

		// 4. Load Wavelet Huffman tables from user's exe
		{
			std::string err;
			wavelet_ = codol::render::wavelet_tables::load_from_executable(report_.exe_path, &err);
			if (!wavelet_)
				report_.warnings.push_back("Wavelet tables load failed: " + err);
			else
				report_.has_wavelet_tables = true;
		}

		// 5. Open encrypted .iwd archives (images, models, scripts, sounds)
		if (report_.has_iwd_key)
		{
			std::string err;
			std::vector<std::string> iwd_warnings;
			if (codol::render::open_image_archives(game_root, iwd_key_, images_, &err, &iwd_warnings))
			{
				report_.image_archive_count = (images_.images ? images_.images->archives().size() : 0)
					+ (images_.stripped ? images_.stripped->archives().size() : 0);
				report_.image_entry_count = (images_.images ? images_.images->entry_count() : 0)
					+ (images_.stripped ? images_.stripped->entry_count() : 0);
			}
			else
			{
				report_.warnings.push_back("Image IWD open warning: " + err);
			}
			for (auto& w : iwd_warnings) report_.warnings.push_back(std::move(w));

			std::vector<std::filesystem::path> dirs = { std::filesystem::path(game_root) / "main" };
			if (platform::dir_exists(game_root + "/main_shared"))
				dirs.push_back(std::filesystem::path(game_root) / "main_shared");

			models_iwd_ = codol::render::iwd_archive_set::open(dirs, &iwd_key_, "models/", {}, nullptr);
			if (models_iwd_) report_.model_archive_count = models_iwd_->archives().size();

			sounds_iwd_ = codol::render::iwd_archive_set::open(dirs, &iwd_key_, "sound/", {}, nullptr);
			if (sounds_iwd_) report_.sound_archive_count = sounds_iwd_->archives().size();

			// sh1x_svr_scripts.iwd is unencrypted plain zip in offline client, plus encrypted stock iwds
			scripts_iwd_ = codol::render::iwd_archive_set::open(dirs, nullptr, "maps/", [](const std::string& fn) {
				return lower_ascii(fn).find("script") != std::string::npos;
			}, nullptr);
			if (scripts_iwd_) report_.script_archive_count = scripts_iwd_->archives().size();
		}

		// 6. Scan FastFile folders (patch overrides stock chinese, matching CODOL load order)
		const std::vector<std::string> ff_dirs = {
			game_root + "/client_shipRetail/chinese",
			game_root + "/client_shipRetail/english",
			game_root + "/client_shipRetail/patch",
			game_root + "/zone/chinese",
			game_root + "/zone/english"
		};

		for (const auto& dir : ff_dirs)
		{
			if (!platform::dir_exists(dir)) continue;
			const auto files = platform::list_dir_files(dir, ".ff");
			for (const auto& fn : files)
			{
				const std::string stem = fn.substr(0, fn.size() - 3);
				const std::string low = lower_ascii(stem);
				const std::string full = dir + "/" + fn;
				fastfile_paths_[low] = full; // later dirs (patch) override earlier ones
			}
		}

		report_.fastfile_count = fastfile_paths_.size();
		for (const auto& [stem, full] : fastfile_paths_)
		{
			// Skip *_load.ff helper zones and shared/common/non-world zones when building playable map catalog
			if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, "_load") == 0) continue;
			if (stem.find("common") != std::string::npos || stem.find("code_") != std::string::npos ||
			    stem.find("patch_") != std::string::npos || stem.find("_postgfx") != std::string::npos ||
			    stem == "mp_playlists" || stem == "mp_weapons") continue;

			std::string cat;
			if (starts_with(stem, "mp_")) cat = "mp";
			else if (starts_with(stem, "zm_")) cat = "zm";
			else if (starts_with(stem, "coop_") || starts_with(stem, "pve_")) cat = "coop";
			else continue;

			MapCatalogEntry entry;
			entry.name = stem;
			entry.ff_path = full;
			entry.category = cat;
			if (std::FILE* f = std::fopen(full.c_str(), "rb"))
			{
				std::fseek(f, 0, SEEK_END);
				const long sz = std::ftell(f);
				if (sz > 0) entry.file_size = static_cast<std::uint64_t>(sz);
				std::fclose(f);
			}
			report_.maps.push_back(std::move(entry));
		}
		std::sort(report_.maps.begin(), report_.maps.end(), [](const MapCatalogEntry& a, const MapCatalogEntry& b) {
			return a.name < b.name;
		});

		mounted_ = true;
		report_.ok = true;
		platform::log_info("Mounted CODOL 1.1.1.13: %zu fastfiles (%zu playable maps), %zu IWD image archives (%zu images)",
			report_.fastfile_count, report_.maps.size(), report_.image_archive_count, report_.image_entry_count);
		return report_;
	}

	std::string AssetManager::find_fastfile_path(const std::string& zone_name) const
	{
		if (platform::file_exists(zone_name)) return zone_name;
		std::string key = lower_ascii(zone_name);
		if (key.size() > 3 && key.compare(key.size() - 3, 3, ".ff") == 0)
			key.resize(key.size() - 3);
		const auto it = fastfile_paths_.find(key);
		return it != fastfile_paths_.end() ? it->second : std::string();
	}

	std::shared_ptr<codol::zone::Zone> AssetManager::load_zone(const std::string& name_or_path, std::string* error)
	{
		if (!mounted_ || !ff_key_.valid())
		{
			if (error) *error = "AssetManager is not mounted with a valid client key";
			return nullptr;
		}
		const std::string path = find_fastfile_path(name_or_path);
		if (path.empty())
		{
			if (error) *error = "FastFile not found: " + name_or_path;
			return nullptr;
		}
		auto zone = std::make_shared<codol::zone::Zone>();
		const auto st = zone->open(path, ff_key_);
		if (!st)
		{
			if (error) *error = st.message;
			return nullptr;
		}
		platform::MemoryStats::instance().add(platform::MemTag::Zones, static_cast<std::int64_t>(zone->stream_size()));
		return zone;
	}

	std::shared_ptr<DecodedTexture> AssetManager::downsample_if_needed(DecodedTexture tex, int max_dim)
	{
		while (max_dim > 0 && (tex.width > max_dim || tex.height > max_dim) && tex.width >= 2 && tex.height >= 2)
		{
			const int nw = std::max(1, tex.width / 2);
			const int nh = std::max(1, tex.height / 2);
			std::vector<std::uint8_t> dst(static_cast<std::size_t>(nw) * nh * 4);
			for (int y = 0; y < nh; ++y)
			{
				for (int x = 0; x < nw; ++x)
				{
					const int sx = x * 2;
					const int sy = y * 2;
					const int sx1 = std::min(sx + 1, tex.width - 1);
					const int sy1 = std::min(sy + 1, tex.height - 1);
					const std::uint8_t* p00 = &tex.rgba[(static_cast<std::size_t>(sy) * tex.width + sx) * 4];
					const std::uint8_t* p10 = &tex.rgba[(static_cast<std::size_t>(sy) * tex.width + sx1) * 4];
					const std::uint8_t* p01 = &tex.rgba[(static_cast<std::size_t>(sy1) * tex.width + sx) * 4];
					const std::uint8_t* p11 = &tex.rgba[(static_cast<std::size_t>(sy1) * tex.width + sx1) * 4];
					std::uint8_t* o = &dst[(static_cast<std::size_t>(y) * nw + x) * 4];
					for (int c = 0; c < 4; ++c)
						o[c] = static_cast<std::uint8_t>((p00[c] + p10[c] + p01[c] + p11[c] + 2) / 4);
				}
			}
			tex.width = nw;
			tex.height = nh;
			tex.rgba = std::move(dst);
		}
		platform::MemoryStats::instance().add(platform::MemTag::Textures, static_cast<std::int64_t>(tex.rgba.size()));
		return std::make_shared<DecodedTexture>(std::move(tex));
	}

	std::shared_ptr<DecodedTexture> AssetManager::get_streamed_texture(const std::string& image_name, int max_dim)
	{
		const std::string clean = lower_ascii(trim_comma(image_name));
		if (clean.empty()) return nullptr;
		const std::string key = clean + "@" + std::to_string(max_dim);
		{
			std::lock_guard<std::mutex> lock(cache_mutex_);
			auto it = texture_cache_.find(key);
			if (it != texture_cache_.end()) return it->second;
		}

		std::vector<std::uint8_t> raw;
		std::string err;
		if (!images_.read(clean, raw, &err))
		{
			std::lock_guard<std::mutex> lock(cache_mutex_);
			texture_cache_.emplace(key, nullptr);
			return nullptr;
		}

		codol::render::iwi_file iwi;
		codol::render::texture_upload upload;
		if (!codol::render::parse_iwi(raw, iwi, &err) ||
		    !codol::render::build_texture_upload(iwi, wavelet_.get(), upload, &err))
		{
			std::lock_guard<std::mutex> lock(cache_mutex_);
			texture_cache_.emplace(key, nullptr);
			return nullptr;
		}

		// Pick the highest-resolution mip level that fits <= max_dim (or smallest if all bigger)
		const codol::render::texture_surface* chosen = upload.surface(0, 0);
		if (max_dim > 0)
		{
			for (int lvl = 0; lvl < upload.levels; ++lvl)
			{
				const auto* s = upload.surface(lvl, 0);
				if (!s) continue;
				chosen = s;
				if (s->width <= max_dim && s->height <= max_dim) break;
			}
		}
		if (!chosen) return nullptr;

		codol::render::rgba_image rgba;
		if (!codol::render::upload_surface_to_rgba(upload, *chosen, rgba, &err))
		{
			std::lock_guard<std::mutex> lock(cache_mutex_);
			texture_cache_.emplace(key, nullptr);
			return nullptr;
		}

		DecodedTexture tex;
		tex.name = clean;
		tex.width = rgba.width;
		tex.height = rgba.height;
		tex.rgba = std::move(rgba.pixels);

		// Quick alpha classification
		bool has_zero_alpha = false;
		bool has_mid_alpha = false;
		for (std::size_t i = 3; i < tex.rgba.size(); i += 16)
		{
			const std::uint8_t a = tex.rgba[i];
			if (a < 20) has_zero_alpha = true;
			else if (a < 235) has_mid_alpha = true;
		}
		tex.is_alpha_tested = has_zero_alpha && !has_mid_alpha;
		tex.is_translucent = has_mid_alpha;

		auto ptr = downsample_if_needed(std::move(tex), max_dim);
		std::lock_guard<std::mutex> lock(cache_mutex_);
		texture_cache_[key] = ptr;
		return ptr;
	}

	std::shared_ptr<DecodedTexture> AssetManager::get_embedded_texture(const codol::zone::Zone& zone, codol::zone::zaddr image_addr, int max_dim)
	{
		if (image_addr == 0) return nullptr;
		codol::zone::ImageView img;
		if (!codol::zone::read_image(zone, image_addr, img))
			return nullptr;

		if (img.streamed || img.pixels.empty())
			return get_streamed_texture(std::string(img.name), max_dim);

		const std::string key = "emb:" + std::string(img.name) + "#" + std::to_string(img.pixels.size()) + "@" + std::to_string(max_dim);
		{
			std::lock_guard<std::mutex> lock(cache_mutex_);
			auto it = texture_cache_.find(key);
			if (it != texture_cache_.end()) return it->second;
		}

		codol::render::embedded_image emb;
		emb.d3d_format = img.loadDef.format;
		emb.level_count = img.loadDef.levelCount;
		emb.flags = img.loadDef.flags;
		emb.width = img.raw.width;
		emb.height = img.raw.height;
		emb.depth = std::max<int>(img.raw.depth, 1);
		emb.map_type = img.raw.mapType;
		emb.data = 0;
		emb.data_size = static_cast<std::uint32_t>(img.pixels.size());

		codol::render::texture_upload upload;
		std::string err;
		if (!codol::render::build_embedded_upload(emb, img.pixels, upload, &err))
			return nullptr;

		const auto* top = upload.surface(0, 0);
		if (!top) return nullptr;

		codol::render::rgba_image rgba;
		if (!codol::render::upload_surface_to_rgba(upload, *top, rgba, &err))
			return nullptr;

		DecodedTexture tex;
		tex.name = std::string(img.name);
		tex.width = rgba.width;
		tex.height = rgba.height;
		tex.rgba = std::move(rgba.pixels);

		auto ptr = downsample_if_needed(std::move(tex), max_dim);
		std::lock_guard<std::mutex> lock(cache_mutex_);
		texture_cache_[key] = ptr;
		return ptr;
	}

	std::shared_ptr<ResolvedMaterial> AssetManager::resolve_material(const codol::zone::Zone& zone, codol::zone::zaddr material_addr, int max_tex_dim)
	{
		if (material_addr == 0) return nullptr;
		const std::string mat_name = trim_comma(zone.asset_name(codol::c1::ASSET_TYPE_MATERIAL, material_addr));
		const std::string cache_key = zone.name() + ":" + mat_name;
		{
			std::lock_guard<std::mutex> lock(cache_mutex_);
			auto it = material_cache_.find(cache_key);
			if (it != material_cache_.end()) return it->second;
		}

		codol::zone::MaterialView mv;
		if (!codol::zone::read_material(zone, material_addr, mv))
			return nullptr;

		auto res = std::make_shared<ResolvedMaterial>();
		res->name = mat_name;
		res->techset_name = std::string(mv.techsetName);
		res->sort_key = mv.raw.sortKey;
		res->camera_region = mv.raw.cameraRegion;

		const std::string low_name = lower_ascii(res->name);
		const std::string low_ts = lower_ascii(res->techset_name);
		res->is_sky = low_name.find("sky") != std::string::npos || low_ts.find("sky") != std::string::npos;
		res->is_decal = low_name.find("decal") != std::string::npos || low_ts.find("decal") != std::string::npos;
		res->is_nodraw = low_name.find("caulk") != std::string::npos ||
		                 low_name.find("clip") != std::string::npos ||
		                 low_name.find("portal") != std::string::npos ||
		                 low_name.find("hint") != std::string::npos ||
		                 low_name.find("trigger") != std::string::npos ||
		                 low_name.find("shadowcaster") != std::string::npos ||
		                 low_name.find("light_shaft") != std::string::npos ||
		                 low_name.find("lightshaft") != std::string::npos ||
		                 low_name.find("volumetric") != std::string::npos ||
		                 low_name.find("glow") != std::string::npos ||
		                 low_name.find("beam") != std::string::npos ||
		                 low_name.find("flare") != std::string::npos ||
		                 low_name.find("corona") != std::string::npos ||
		                 low_name.find("fog") != std::string::npos ||
		                 low_name.find("ray") != std::string::npos ||
		                 low_name.find("dust") != std::string::npos ||
		                 low_ts.find("effect") != std::string::npos ||
		                 low_ts.find("unlit_add") != std::string::npos ||
		                 low_ts.find("_add_") != std::string::npos;
		res->is_translucent = res->camera_region >= 2 ||
		                      low_ts.find("blend") != std::string::npos ||
		                      low_ts.find("alpha") != std::string::npos ||
		                      low_name.find("glass") != std::string::npos ||
		                      low_name.find("water") != std::string::npos ||
		                      low_name.find("window") != std::string::npos;

		// Select diffuse colorMap (semantic == 2 COLOR_MAP, or nameStart=='c', or fallback first image)
		codol::zone::zaddr best_img_addr = 0;
		std::string best_img_name;
		int best_score = -1;

		for (const auto& t : mv.textures)
		{
			if (t.imageName.empty()) continue;
			int score = 0;
			if (t.raw.semantic == codol::render::texture_semantic::COLOR_MAP) score += 100;
			if (t.raw.nameStart == 'c' && t.raw.nameEnd == 'p') score += 50; // colorMap
			if (t.raw.semantic == codol::render::texture_semantic::NORMAL_MAP)
			{
				res->normal_map_name = trim_comma(t.imageName);
				continue;
			}
			if (t.raw.semantic == codol::render::texture_semantic::SPECULAR_MAP) continue;
			if (score > best_score)
			{
				best_score = score;
				best_img_addr = t.image;
				best_img_name = trim_comma(t.imageName);
			}
		}

		res->color_map_name = best_img_name;
		if (!best_img_name.empty())
		{
			// Try streamed .iwi first, then embedded GfxImage inside the zone
			res->color_texture = get_streamed_texture(best_img_name, max_tex_dim);
			if (!res->color_texture && best_img_addr != 0)
				res->color_texture = get_embedded_texture(zone, best_img_addr, max_tex_dim);
		}

		std::lock_guard<std::mutex> lock(cache_mutex_);
		material_cache_[cache_key] = res;
		return res;
	}

	bool AssetManager::read_script_file(const std::string& rel_path, std::vector<std::uint8_t>& out) const
	{
		// First check loose override in main/sh1x_rawfiles/
		if (!report_.game_root.empty())
		{
			const std::string loose = report_.game_root + "/main/sh1x_rawfiles/" + rel_path;
			if (platform::file_exists(loose))
				return platform::read_file_bytes(loose, out);
		}
		if (scripts_iwd_)
		{
			std::string err;
			return scripts_iwd_->read(rel_path, out, &err);
		}
		return false;
	}

	bool AssetManager::read_model_file(const std::string& rel_path, std::vector<std::uint8_t>& out) const
	{
		if (!models_iwd_) return false;
		std::string err;
		return models_iwd_->read(rel_path, out, &err);
	}

	bool AssetManager::read_sound_file(const std::string& rel_path, std::vector<std::uint8_t>& out) const
	{
		if (!sounds_iwd_) return false;
		std::string err;
		return sounds_iwd_->read(rel_path, out, &err);
	}

	void AssetManager::clear_caches()
	{
		std::lock_guard<std::mutex> lock(cache_mutex_);
		texture_cache_.clear();
		material_cache_.clear();
	}

	std::size_t AssetManager::cached_texture_count() const
	{
		std::lock_guard<std::mutex> lock(cache_mutex_);
		return texture_cache_.size();
	}

	std::size_t AssetManager::cached_material_count() const
	{
		std::lock_guard<std::mutex> lock(cache_mutex_);
		return material_cache_.size();
	}
}
