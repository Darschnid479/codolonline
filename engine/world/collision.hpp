#pragma once

// Project Dragon — Phase 4: Authentic clipMap_t Collision System
// Performs swept AABB box traces (CM_BoxTrace) and raycasts (CM_RayTrace) directly
// against Call of Duty Online 1.1.1.13 cbrush_t axial + bevel planes decoded from FastFile v334.

#include "codol_zone/assets.hpp"
#include "math3d.hpp"

#include <cstdint>
#include <vector>

namespace dragon::world
{
	inline constexpr std::int32_t CONTENTS_SOLID      = 0x00000001;
	inline constexpr std::int32_t CONTENTS_PLAYERCLIP = 0x00010000;
	inline constexpr std::int32_t MASK_PLAYERSOLID    = CONTENTS_SOLID | CONTENTS_PLAYERCLIP;

	struct TraceResult
	{
		float fraction = 1.0f;
		math::Vec3 endpos{};
		math::Vec3 normal{ 0.0f, 0.0f, 1.0f };
		bool start_solid = false;
		bool all_solid = false;
		std::int32_t contents = 0;
		int brush_index = -1;
	};

	struct CollBrush
	{
		math::AABB bounds;
		std::int32_t contents = 0;
		struct SidePlane
		{
			math::Vec3 normal;
			float dist;
		};
		std::vector<SidePlane> planes;
	};

	class CollisionWorld
	{
	public:
		bool load_from_zone(const codol::zone::Zone& zone);
		void add_synthetic_box(const math::Vec3& mins, const math::Vec3& maxs, std::int32_t contents = CONTENTS_SOLID);
		void clear();

		std::size_t brush_count() const { return brushes_.size(); }
		bool loaded() const { return !brushes_.empty(); }

		// Swept AABB trace (start -> end with box extents mins..maxs) against all matching brushes
		TraceResult box_trace(const math::Vec3& start, const math::Vec3& end,
			const math::Vec3& mins, const math::Vec3& maxs, std::int32_t content_mask = MASK_PLAYERSOLID) const;

		// Point / weapon hitscan ray trace
		TraceResult ray_trace(const math::Vec3& start, const math::Vec3& end, std::int32_t content_mask = CONTENTS_SOLID) const;

	private:
		void trace_brush(int idx, const CollBrush& brush, const math::Vec3& start, const math::Vec3& end,
			const math::Vec3& mins, const math::Vec3& maxs, TraceResult& out) const;

		std::vector<CollBrush> brushes_;
	};
}
