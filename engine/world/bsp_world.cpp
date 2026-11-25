#include "bsp_world.hpp"
#include "../platform/platform.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace dragon::world
{
	namespace
	{
		inline std::uint16_t rd_u16(const std::uint8_t* p)
		{
			return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
		}
		inline std::uint32_t rd_u32(const std::uint8_t* p)
		{
			return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
				| (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
		}
		inline float rd_f32(const std::uint8_t* p)
		{
			const std::uint32_t u = rd_u32(p);
			float f;
			std::memcpy(&f, &u, sizeof(f));
			return f;
		}
	}

	bool WorldScene::load_from_zone(const codol::zone::Zone& zone, assets::AssetManager& asset_mgr, int max_tex_dim, std::string* error)
	{
		*this = {};
		map_name = zone.name();

		const auto world_addr = codol::zone::first_asset(zone, codol::c1::ASSET_TYPE_GFXWORLD);
		if (!world_addr)
		{
			if (error) *error = "Zone '" + zone.name() + "' does not contain an ASSET_TYPE_GFXWORLD asset";
			return false;
		}

		codol::zone::GfxWorldView gw;
		if (!codol::zone::read_gfx_world(zone, world_addr, gw))
		{
			if (error) *error = "Failed to decode GfxWorld header from '" + zone.name() + "'";
			return false;
		}

		bsp_name = std::string(gw.name);
		world_bounds = math::AABB::from_mid_half(
			{ gw.raw.bounds.midPoint[0], gw.raw.bounds.midPoint[1], gw.raw.bounds.midPoint[2] },
			{ gw.raw.bounds.halfSize[0], gw.raw.bounds.halfSize[1], gw.raw.bounds.halfSize[2] });

		raw_world_vertex_count = gw.raw.draw.vertexCount;
		raw_world_index_count = gw.raw.draw.indexCount;
		raw_surface_count = static_cast<std::uint32_t>(gw.surfaces.size());
		raw_static_model_count = static_cast<std::uint32_t>(gw.staticModels.size());

		// 1. Decode GfxWorldVertex stream (44 bytes each)
		const std::size_t vcount = gw.vertexBytes.size() / sizeof(codol::c1::GfxWorldVertex);
		vertices.resize(vcount);
		for (std::size_t i = 0; i < vcount; ++i)
		{
			codol::c1::GfxWorldVertex src{};
			std::memcpy(&src, gw.vertexBytes.data() + i * sizeof(codol::c1::GfxWorldVertex), sizeof(src));
			float nrm[3];
			codol::zone::unpack_unit_vector(src.normal, nrm);
			RenderVertex& dst = vertices[i];
			dst.x = src.xyz[0];
			dst.y = src.xyz[1];
			dst.z = src.xyz[2];
			dst.nx = nrm[0];
			dst.ny = nrm[1];
			dst.nz = nrm[2];
			dst.u = src.texCoord[0];
			dst.v = src.texCoord[1];
			dst.lu = src.lmapCoord[0];
			dst.lv = src.lmapCoord[1];
			// D3DCOLOR is BGRA in memory -> convert to RGBA
			dst.b = static_cast<std::uint8_t>((src.color >> 0) & 0xFF);
			dst.g = static_cast<std::uint8_t>((src.color >> 8) & 0xFF);
			dst.r = static_cast<std::uint8_t>((src.color >> 16) & 0xFF);
			dst.a = static_cast<std::uint8_t>((src.color >> 24) & 0xFF);
		}

		// 2. Decode embedded lightmaps (pairs of primary + secondary; take primary)
		if (gw.raw.draw.lightmapCount > 0 && gw.raw.draw.lightmaps != 0)
		{
			const auto ptrs = zone.get_array<codol::zone::zaddr>(gw.raw.draw.lightmaps, static_cast<std::size_t>(gw.raw.draw.lightmapCount) * 2u);
			lightmaps.reserve(static_cast<std::size_t>(gw.raw.draw.lightmapCount));
			for (std::size_t i = 0; i + 1 < ptrs.size(); i += 2)
			{
				lightmaps.push_back(asset_mgr.get_embedded_texture(zone, ptrs[i], 256));
			}
		}

		// 3. Decode GfxCell list
		cells.reserve(gw.cells.size());
		for (const auto& c : gw.cells)
		{
			BspCell cell;
			cell.center = { c.raw.bounds.midPoint[0], c.raw.bounds.midPoint[1], c.raw.bounds.midPoint[2] };
			cell.half = { c.raw.bounds.halfSize[0], c.raw.bounds.halfSize[1], c.raw.bounds.halfSize[2] };
			cell.portal_count = c.raw.portalCount;
			cells.push_back(cell);
		}

		// 4. Decode GfxSurface list + resolve materials + build absolute index buffer
		const auto bounds_arr = zone.get_array<codol::c1::Bounds>(gw.raw.dpvs.surfacesBounds, gw.surfaces.size());
		const std::uint32_t emissive_begin = gw.raw.dpvs.emissiveSurfsBegin;
		const std::uint32_t emissive_end = gw.raw.dpvs.emissiveSurfsEnd;
		const std::uint32_t trans_begin = gw.raw.dpvs.litTransSurfsBegin;
		const std::uint32_t trans_end = gw.raw.dpvs.litTransSurfsEnd;

		surfaces.reserve(gw.surfaces.size());
		std::size_t total_world_indices = 0;
		for (const auto& s : gw.surfaces) total_world_indices += static_cast<std::size_t>(s.raw.triCount) * 3u;
		indices.reserve(total_world_indices);

		for (std::size_t i = 0; i < gw.surfaces.size(); ++i)
		{
			const auto& raw = gw.surfaces[i].raw;
			WorldSurface ws;
			ws.index = static_cast<int>(i);
			ws.first_vertex = raw.firstVertex;
			ws.vertex_count = raw.vertexCount;
			ws.tri_count = raw.triCount;
			ws.lightmap_index = raw.lightmapIndex;
			ws.emissive = (i >= emissive_begin && i < emissive_end);
			ws.translucent = (i >= trans_begin && i < trans_end);
			ws.first_index = static_cast<std::uint32_t>(indices.size());

			if (i < bounds_arr.size())
			{
				ws.bounds_center = { bounds_arr[i].midPoint[0], bounds_arr[i].midPoint[1], bounds_arr[i].midPoint[2] };
				ws.bounds_half = { bounds_arr[i].halfSize[0], bounds_arr[i].halfSize[1], bounds_arr[i].halfSize[2] };
			}
			else
			{
				ws.bounds_center = world_bounds.center();
				ws.bounds_half = world_bounds.half_size();
			}

			ws.material = asset_mgr.resolve_material(zone, raw.material, max_tex_dim);
			if (ws.material)
			{
				ws.is_sky = ws.material->is_sky;
				ws.is_nodraw = ws.material->is_nodraw;
				if (ws.material->is_translucent) ws.translucent = true;
				if (ws.material->color_texture && ws.material->color_texture->is_valid())
					++resolved_textured_surfaces;
			}

			const std::size_t icount = static_cast<std::size_t>(raw.triCount) * 3u;
			for (std::size_t k = 0; k < icount; ++k)
			{
				const std::size_t byte_off = 2u * (static_cast<std::size_t>(raw.baseIndex) + k);
				std::uint32_t vidx = raw.firstVertex;
				if (byte_off + 1 < gw.indexBytes.size())
					vidx += rd_u16(gw.indexBytes.data() + byte_off);
				if (vidx >= vertices.size()) vidx = 0;
				indices.push_back(vidx);
			}
			surfaces.push_back(std::move(ws));
		}

		// 5. Decode ComWorld primary lights (Sun + Spot + Omni)
		if (const auto com_addr = codol::zone::first_asset(zone, codol::c1::ASSET_TYPE_COMWORLD))
		{
			codol::zone::ComWorldView com;
			if (codol::zone::read_com_world(zone, com_addr, com) && com.primaryLightStride >= 68)
			{
				for (std::uint32_t i = 0; i < com.raw.primaryLightCount; ++i)
				{
					const std::uint8_t* l = com.primaryLightBytes.data() + static_cast<std::size_t>(i) * com.primaryLightStride;
					PrimaryLight pl;
					pl.type = l[0];
					pl.color = { rd_f32(l + 4), rd_f32(l + 8), rd_f32(l + 12) };
					pl.direction = math::normalize({ rd_f32(l + 16), rd_f32(l + 20), rd_f32(l + 24) });
					pl.origin = { rd_f32(l + 28), rd_f32(l + 32), rd_f32(l + 36) };
					pl.radius = rd_f32(l + 40);
					if (pl.type == 1 && !has_sun && i <= gw.raw.lastSunPrimaryLightIndex)
					{
						sun_light = pl;
						has_sun = true;
					}
					primary_lights.push_back(pl);
				}
			}
		}

		// 6. Decode inline XModel static model instances (LOD 0) into renderable world meshes
		// Cache decoded ModelView by model zaddr so repeated props decode once.
		std::unordered_map<codol::zone::zaddr, codol::zone::ModelView> model_cache;
		constexpr std::size_t MAX_STATIC_INSTANCES_BAKE = 1500; // cap memory budget on PS4
		std::size_t baked = 0;
		for (const auto& inst : gw.staticModels)
		{
			if (baked >= MAX_STATIC_INSTANCES_BAKE) break;
			if (inst.raw.model == 0) continue;

			auto it = model_cache.find(inst.raw.model);
			if (it == model_cache.end())
			{
				codol::zone::ModelView mv;
				if (!codol::zone::read_model(zone, inst.raw.model, mv, true))
				{
					model_cache.emplace(inst.raw.model, codol::zone::ModelView{});
					continue;
				}
				it = model_cache.emplace(inst.raw.model, std::move(mv)).first;
			}
			const auto& mv = it->second;
			if (mv.lods.empty() || mv.lods[0].surfs.surfaces.empty()) continue;

			const float s = inst.raw.scale > 1e-4f ? inst.raw.scale : 1.0f;
			const auto& a = inst.raw.axis;
			const auto& o = inst.raw.origin;

			const auto& lod0 = mv.lods[0];
			for (std::size_t si = 0; si < lod0.surfs.surfaces.size(); ++si)
			{
				const auto& surf = lod0.surfs.surfaces[si];
				if (surf.vertCount == 0 || surf.triCount == 0) continue;

				const auto decoded_verts = surf.vertices();
				const auto decoded_tris = surf.indices();
				if (decoded_verts.empty() || decoded_tris.empty()) continue;

				StaticModelMesh mesh;
				mesh.model_name = std::string(mv.name);
				mesh.first_vertex = static_cast<std::uint32_t>(vertices.size());
				mesh.first_index = static_cast<std::uint32_t>(indices.size());
				mesh.tri_count = static_cast<std::uint32_t>(decoded_tris.size() / 3u);
				mesh.cull_dist = static_cast<float>(inst.raw.cullDist);

				const std::size_t mat_idx = static_cast<std::size_t>(lod0.surfIndex) + si;
				if (mat_idx < mv.materials.size())
					mesh.material = asset_mgr.resolve_material(zone, mv.materials[mat_idx], max_tex_dim);

				math::AABB mb;
				for (const auto& sv : decoded_verts)
				{
					const float lx = sv.position[0] * s;
					const float ly = sv.position[1] * s;
					const float lz = sv.position[2] * s;
					const float wx = o[0] + a[0][0] * lx + a[1][0] * ly + a[2][0] * lz;
					const float wy = o[1] + a[0][1] * lx + a[1][1] * ly + a[2][1] * lz;
					const float wz = o[2] + a[0][2] * lx + a[1][2] * ly + a[2][2] * lz;
					const float nx = a[0][0] * sv.normal[0] + a[1][0] * sv.normal[1] + a[2][0] * sv.normal[2];
					const float ny = a[0][1] * sv.normal[0] + a[1][1] * sv.normal[1] + a[2][1] * sv.normal[2];
					const float nz = a[0][2] * sv.normal[0] + a[1][2] * sv.normal[1] + a[2][2] * sv.normal[2];

					RenderVertex rv{};
					rv.x = wx; rv.y = wy; rv.z = wz;
					rv.nx = nx; rv.ny = ny; rv.nz = nz;
					rv.u = sv.uv[0]; rv.v = sv.uv[1];
					rv.lu = 0.0f; rv.lv = 0.0f;
					rv.r = sv.color[0]; rv.g = sv.color[1]; rv.b = sv.color[2]; rv.a = sv.color[3];
					vertices.push_back(rv);
					mb.expand({ wx, wy, wz });
				}
				for (const std::uint16_t idx : decoded_tris)
					indices.push_back(mesh.first_vertex + idx);

				mesh.bounds_center = mb.center();
				mesh.bounds_half = mb.half_size();
				static_model_meshes.push_back(std::move(mesh));
			}
			++baked;
		}
		decoded_static_model_instances = static_cast<std::uint32_t>(baked);

		platform::MemoryStats::instance().add(
			platform::MemTag::Geometry,
			static_cast<std::int64_t>(vertices.size() * sizeof(RenderVertex) + indices.size() * sizeof(std::uint32_t)));

		platform::log_info(
			"Loaded GfxWorld '%s': %u world verts, %u surfaces (%u textured), %u lightmaps, %u cells, %u/%u static models baked",
			bsp_name.c_str(), raw_world_vertex_count, raw_surface_count, resolved_textured_surfaces,
			static_cast<unsigned>(lightmaps.size()), static_cast<unsigned>(cells.size()),
			decoded_static_model_instances, raw_static_model_count);
		return true;
	}

	int WorldScene::find_cell(const math::Vec3& p) const
	{
		for (std::size_t i = 0; i < cells.size(); ++i)
		{
			const auto& c = cells[i];
			if (std::fabs(p.x - c.center.x) <= c.half.x &&
			    std::fabs(p.y - c.center.y) <= c.half.y &&
			    std::fabs(p.z - c.center.z) <= c.half.z)
			{
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	void WorldScene::collect_visible_surfaces(const math::Frustum& frustum, const math::Vec3& eye, bool enable_culling,
		bool include_static_models, std::vector<const WorldSurface*>& out_world,
		std::vector<const StaticModelMesh*>& out_models) const
	{
		out_world.clear();
		out_models.clear();
		out_world.reserve(surfaces.size());

		for (const auto& s : surfaces)
		{
			if (s.tri_count == 0 || s.is_nodraw) continue;
			if (enable_culling && !frustum.intersects_aabb(s.bounds_center, s.bounds_half))
				continue;
			out_world.push_back(&s);
		}

		if (include_static_models)
		{
			out_models.reserve(static_model_meshes.size() / 2 + 1);
			for (const auto& m : static_model_meshes)
			{
				if (m.tri_count == 0) continue;
				if (m.cull_dist > 1.0f)
				{
					const float d2 = math::length_sq(m.bounds_center - eye);
					if (d2 > m.cull_dist * m.cull_dist) continue;
				}
				if (enable_culling && !frustum.intersects_aabb(m.bounds_center, m.bounds_half))
					continue;
				out_models.push_back(&m);
			}
		}
	}
}
