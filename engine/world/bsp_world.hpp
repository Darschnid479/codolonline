#pragma once

// Project Dragon — Phase 4: COD Online 1.1.1.13 GfxWorld & BSP World Scene
// Decodes real GfxWorld vertices, indices, surfaces, lightmaps, static models,
// brush models, cells/portals, and ComWorld primary lights straight from FastFile v334.

#include "../assets/asset_manager.hpp"
#include "math3d.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dragon::world
{
	struct RenderVertex
	{
		float x, y, z;
		float nx, ny, nz;
		float u, v;
		float lu, lv;          // lightmap UV
		std::uint8_t r, g, b, a;
	};

	struct WorldSurface
	{
		int index = 0;
		std::uint32_t first_vertex = 0;
		std::uint32_t vertex_count = 0;
		std::uint32_t first_index = 0; // index into WorldScene::indices (absolute vertex indices)
		std::uint32_t tri_count = 0;
		std::uint8_t lightmap_index = 255;
		bool emissive = false;
		bool translucent = false;
		bool is_sky = false;
		bool is_nodraw = false;
		math::Vec3 bounds_center{};
		math::Vec3 bounds_half{};
		std::shared_ptr<assets::ResolvedMaterial> material;
	};

	struct PrimaryLight
	{
		std::uint8_t type = 0; // 1 = directional sun, 2 = spot, 3 = omni
		math::Vec3 color{ 1.0f, 0.96f, 0.90f };
		math::Vec3 direction{ -0.45f, -0.35f, -0.82f };
		math::Vec3 origin{};
		float radius = 0.0f;
	};

	struct BspCell
	{
		math::Vec3 center{};
		math::Vec3 half{};
		int portal_count = 0;
	};

	struct StaticModelMesh
	{
		std::string model_name;
		std::uint32_t first_vertex = 0;
		std::uint32_t first_index = 0;
		std::uint32_t tri_count = 0;
		math::Vec3 bounds_center{};
		math::Vec3 bounds_half{};
		float cull_dist = 0.0f;
		std::shared_ptr<assets::ResolvedMaterial> material;
	};

	struct WorldScene
	{
		std::string map_name;
		std::string bsp_name;
		math::AABB world_bounds;

		// Unified vertex & index arrays (world surfaces + baked static model LOD0 instances)
		std::vector<RenderVertex> vertices;
		std::vector<std::uint32_t> indices;

		std::vector<WorldSurface> surfaces;
		std::vector<StaticModelMesh> static_model_meshes;
		std::vector<std::shared_ptr<assets::DecodedTexture>> lightmaps;
		std::vector<BspCell> cells;
		std::vector<PrimaryLight> primary_lights;

		PrimaryLight sun_light;
		bool has_sun = false;

		// Summary counts from original GfxWorld
		std::uint32_t raw_world_vertex_count = 0;
		std::uint32_t raw_world_index_count = 0;
		std::uint32_t raw_surface_count = 0;
		std::uint32_t raw_static_model_count = 0;
		std::uint32_t decoded_static_model_instances = 0;
		std::uint32_t resolved_textured_surfaces = 0;

		bool load_from_zone(const codol::zone::Zone& zone, assets::AssetManager& asset_mgr, int max_tex_dim = 256, std::string* error = nullptr);

		// Frustum & BSP visibility culling
		void collect_visible_surfaces(const math::Frustum& frustum, const math::Vec3& eye, bool enable_culling,
			bool include_static_models, std::vector<const WorldSurface*>& out_world,
			std::vector<const StaticModelMesh*>& out_models) const;

		// Find which GfxCell contains point (-1 if outside)
		int find_cell(const math::Vec3& p) const;
	};
}
