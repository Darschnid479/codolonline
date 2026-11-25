#include "collision.hpp"
#include "../platform/platform.hpp"

#include <algorithm>
#include <cmath>

namespace dragon::world
{
	void CollisionWorld::clear()
	{
		brushes_.clear();
	}

	void CollisionWorld::add_synthetic_box(const math::Vec3& mins, const math::Vec3& maxs, std::int32_t contents)
	{
		CollBrush b;
		b.bounds = { mins, maxs };
		b.contents = contents;
		b.planes = {
			{ { -1,  0,  0 }, -mins.x },
			{ {  1,  0,  0 },  maxs.x },
			{ {  0, -1,  0 }, -mins.y },
			{ {  0,  1,  0 },  maxs.y },
			{ {  0,  0, -1 }, -mins.z },
			{ {  0,  0,  1 },  maxs.z }
		};
		brushes_.push_back(std::move(b));
	}

	bool CollisionWorld::load_from_zone(const codol::zone::Zone& zone)
	{
		brushes_.clear();
		codol::zone::zaddr addr = codol::zone::first_asset(zone, codol::c1::ASSET_TYPE_CLIPMAP_MP);
		if (!addr) addr = codol::zone::first_asset(zone, codol::c1::ASSET_TYPE_CLIPMAP_SP);
		if (!addr) return false;

		codol::zone::ClipMapView cm;
		if (!codol::zone::read_clip_map(zone, addr, cm))
			return false;

		brushes_.reserve(cm.brushes.size());
		for (const auto& src : cm.brushes)
		{
			CollBrush b;
			b.contents = src.contents;
			b.bounds = math::AABB::from_mid_half(
				{ src.bounds.midPoint[0], src.bounds.midPoint[1], src.bounds.midPoint[2] },
				{ src.bounds.halfSize[0], src.bounds.halfSize[1], src.bounds.halfSize[2] });
			b.planes.reserve(src.sides.size());
			for (const auto& s : src.sides)
			{
				b.planes.push_back({
					{ s.plane.normal[0], s.plane.normal[1], s.plane.normal[2] },
					s.plane.dist
				});
			}
			// Ensure axial bounding planes exist even if sides array omitted axial planes
			if (b.planes.size() < 6)
			{
				b.planes.push_back({ { -1,  0,  0 }, -b.bounds.mins.x });
				b.planes.push_back({ {  1,  0,  0 },  b.bounds.maxs.x });
				b.planes.push_back({ {  0, -1,  0 }, -b.bounds.mins.y });
				b.planes.push_back({ {  0,  1,  0 },  b.bounds.maxs.y });
				b.planes.push_back({ {  0,  0, -1 }, -b.bounds.mins.z });
				b.planes.push_back({ {  0,  0,  1 },  b.bounds.maxs.z });
			}
			brushes_.push_back(std::move(b));
		}

		platform::MemoryStats::instance().add(
			platform::MemTag::Collision,
			static_cast<std::int64_t>(brushes_.size() * sizeof(CollBrush)));

		platform::log_info("Loaded ClipMap '%s': %zu brushes, %zu planes, %zu submodels",
			std::string(cm.name).c_str(), brushes_.size(), cm.planes.size(), cm.subModels.size());
		return !brushes_.empty();
	}

	void CollisionWorld::trace_brush(int idx, const CollBrush& brush, const math::Vec3& start, const math::Vec3& end,
		const math::Vec3& mins, const math::Vec3& maxs, TraceResult& out) const
	{
		constexpr float DIST_EPSILON = 0.03125f; // Quake3 / IW4 exact 1/32 unit clip epsilon
		float enter_frac = -1.0f;
		float leave_frac = 1.0f;
		bool start_out = false;
		bool get_out = false;
		math::Vec3 clip_normal{ 0.0f, 0.0f, 1.0f };

		for (const auto& p : brush.planes)
		{
			// Expand plane outward by AABB corner closest to plane
			const math::Vec3 offset{
				p.normal.x < 0.0f ? maxs.x : mins.x,
				p.normal.y < 0.0f ? maxs.y : mins.y,
				p.normal.z < 0.0f ? maxs.z : mins.z
			};
			const float dist = p.dist - math::dot(offset, p.normal);
			const float d1 = math::dot(start, p.normal) - dist;
			const float d2 = math::dot(end, p.normal) - dist;

			if (d2 > 0.0f) get_out = true;
			if (d1 > 0.0f) start_out = true;

			// Completely in front of this plane -> never enters brush
			if (d1 > 0.0f && (d2 >= DIST_EPSILON || d2 >= d1))
				return;

			// Completely behind this plane
			if (d1 <= 0.0f && d2 <= 0.0f)
				continue;

			if (d1 > d2)
			{
				// Entering brush
				float f = (d1 - DIST_EPSILON) / (d1 - d2);
				if (f < 0.0f) f = 0.0f;
				if (f > enter_frac)
				{
					enter_frac = f;
					clip_normal = p.normal;
				}
			}
			else
			{
				// Leaving brush
				const float f = (d1 + DIST_EPSILON) / (d1 - d2);
				if (f < leave_frac)
					leave_frac = f;
			}
		}

		if (!start_out)
		{
			out.start_solid = true;
			if (!get_out)
			{
				out.all_solid = true;
				out.fraction = 0.0f;
				out.contents = brush.contents;
				out.brush_index = idx;
			}
			return;
		}

		if (enter_frac < leave_frac && enter_frac > -1.0f && enter_frac < out.fraction)
		{
			out.fraction = std::max(0.0f, enter_frac);
			out.normal = clip_normal;
			out.contents = brush.contents;
			out.brush_index = idx;
		}
	}

	TraceResult CollisionWorld::box_trace(const math::Vec3& start, const math::Vec3& end,
		const math::Vec3& mins, const math::Vec3& maxs, std::int32_t content_mask) const
	{
		TraceResult res;
		res.fraction = 1.0f;
		res.endpos = end;

		// Swept AABB broadphase bounds
		const math::Vec3 sweep_min = math::vmin(start, end) + mins - math::Vec3{ 1.0f, 1.0f, 1.0f };
		const math::Vec3 sweep_max = math::vmax(start, end) + maxs + math::Vec3{ 1.0f, 1.0f, 1.0f };

		for (std::size_t i = 0; i < brushes_.size(); ++i)
		{
			const CollBrush& b = brushes_[i];
			if ((b.contents & content_mask) == 0) continue;
			if (b.bounds.maxs.x < sweep_min.x || b.bounds.mins.x > sweep_max.x ||
			    b.bounds.maxs.y < sweep_min.y || b.bounds.mins.y > sweep_max.y ||
			    b.bounds.maxs.z < sweep_min.z || b.bounds.mins.z > sweep_max.z)
			{
				continue;
			}
			trace_brush(static_cast<int>(i), b, start, end, mins, maxs, res);
			if (res.all_solid) break;
		}

		res.endpos = start + (end - start) * res.fraction;
		return res;
	}

	TraceResult CollisionWorld::ray_trace(const math::Vec3& start, const math::Vec3& end, std::int32_t content_mask) const
	{
		return box_trace(start, end, { 0, 0, 0 }, { 0, 0, 0 }, content_mask);
	}
}
