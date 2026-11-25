#pragma once

// Project Dragon — Phase 3: Original Call of Duty Online 1.1.1.13 Asset Manager
// Reads user-provided CODOL 1.1.1.13 files natively on PS4 & PC:
//   - FastFile v334 (IWff0100 signed & IWffu100 unsigned) via portable codol_zone
//   - Encrypted .iwd archives (AES-192-CTR BE) + .iwi (DXT1/3/5, RGBA, Wavelet)
//   - Runtime key & wavelet-table extraction from user's codoMP_client_shipRetail.exe
//   - Zero hardcoded proprietary keys or redistributed assets.

#include "codol_render/iwd.hpp"
#include "codol_render/iwi.hpp"
#include "codol_render/material.hpp"
#include "codol_render/texture.hpp"
#include "codol_zone/assets.hpp"
#include "codol_zone/c1_layouts.hpp"
#include "codol_zone/fastfile.hpp"
#include "codol_zone/zone.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dragon::assets
{
	struct DecodedTexture
	{
		std::string name;
		int width = 0;
		int height = 0;
		std::vector<std::uint8_t> rgba; // width * height * 4 (R, G, B, A)
		bool is_alpha_tested = false;
		bool is_translucent = false;
		bool is_valid() const { return width > 0 && height > 0 && rgba.size() == static_cast<std::size_t>(width) * height * 4; }
	};

	struct ResolvedMaterial
	{
		std::string name;
		std::string techset_name;
		std::string color_map_name;
		std::string normal_map_name;
		std::uint8_t sort_key = 0;
		std::uint8_t camera_region = 0;
		bool is_sky = false;
		bool is_decal = false;
		bool is_translucent = false;
		bool is_nodraw = false;
		std::shared_ptr<DecodedTexture> color_texture;
	};

	struct MapCatalogEntry
	{
		std::string name;       // e.g. "mp_killhouse_sh"
		std::string ff_path;    // full path to .ff
		std::string category;   // "mp", "zm", "coop", "other"
		std::uint64_t file_size = 0;
	};

	struct MountReport
	{
		bool ok = false;
		std::string game_root;
		std::string exe_path;
		bool has_fastfile_key = false;
		bool has_iwd_key = false;
		bool has_wavelet_tables = false;
		std::size_t image_archive_count = 0;
		std::size_t image_entry_count = 0;
		std::size_t model_archive_count = 0;
		std::size_t script_archive_count = 0;
		std::size_t sound_archive_count = 0;
		std::size_t fastfile_count = 0;
		std::vector<MapCatalogEntry> maps;
		std::vector<std::string> warnings;
		std::string error;
	};

	class AssetManager
	{
	public:
		AssetManager() = default;
		~AssetManager() = default;

		// Mount a user-provided Call of Duty Online 1.1.1.13 installation folder
		MountReport mount(const std::string& game_root);
		bool is_mounted() const { return mounted_; }
		const MountReport& report() const { return report_; }

		// Load a FastFile zone by name (e.g. "mp_killhouse_sh") or full path
		std::shared_ptr<codol::zone::Zone> load_zone(const std::string& name_or_path, std::string* error = nullptr);
		std::string find_fastfile_path(const std::string& zone_name) const;

		// Decode a streamed .iwi image (from main/*.iwd) into RGBA8
		std::shared_ptr<DecodedTexture> get_streamed_texture(const std::string& image_name, int max_dim = 512);

		// Decode an embedded GfxImage (e.g. lightmap or inline texture inside a Zone) into RGBA8
		std::shared_ptr<DecodedTexture> get_embedded_texture(const codol::zone::Zone& zone, codol::zone::zaddr image_addr, int max_dim = 512);

		// Resolve a Material header from a loaded Zone (finds diffuse colorMap, loads texture)
		std::shared_ptr<ResolvedMaterial> resolve_material(const codol::zone::Zone& zone, codol::zone::zaddr material_addr, int max_tex_dim = 256);

		// Read raw file bytes from script/sound/model IWDs (e.g. "maps/mp/_utility.gsc" or "models/...")
		bool read_script_file(const std::string& rel_path, std::vector<std::uint8_t>& out) const;
		bool read_model_file(const std::string& rel_path, std::vector<std::uint8_t>& out) const;
		bool read_sound_file(const std::string& rel_path, std::vector<std::uint8_t>& out) const;

		// Accessors for validation tools
		const codol::zone::ClientKey& fastfile_key() const { return ff_key_; }
		const codol::render::iwd_key& iwd_key() const { return iwd_key_; }
		const codol::render::image_archive_sets& image_archives() const { return images_; }
		const codol::render::wavelet_tables* wavelet() const { return wavelet_.get(); }

		void clear_caches();
		std::size_t cached_texture_count() const;
		std::size_t cached_material_count() const;

	private:
		std::shared_ptr<DecodedTexture> downsample_if_needed(DecodedTexture tex, int max_dim);

		bool mounted_ = false;
		MountReport report_;
		codol::zone::ClientKey ff_key_;
		codol::render::iwd_key iwd_key_{};
		std::shared_ptr<const codol::render::wavelet_tables> wavelet_;
		codol::render::image_archive_sets images_;
		std::unique_ptr<codol::render::iwd_archive_set> models_iwd_;
		std::unique_ptr<codol::render::iwd_archive_set> scripts_iwd_;
		std::unique_ptr<codol::render::iwd_archive_set> sounds_iwd_;

		std::unordered_map<std::string, std::string> fastfile_paths_; // lowercase stem -> full path

		mutable std::mutex cache_mutex_;
		std::unordered_map<std::string, std::shared_ptr<DecodedTexture>> texture_cache_;
		std::unordered_map<std::string, std::shared_ptr<ResolvedMaterial>> material_cache_;
	};
}
