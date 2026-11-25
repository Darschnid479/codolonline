#pragma once

// Project Dragon — Phase 4: Authentic MapEnts Entity Parser
// Parses Call of Duty Online 1.1.1.13 MapEnts key-value blocks directly from FastFile v334.

#include "codol_zone/assets.hpp"
#include "math3d.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace dragon::world
{
	struct MapEntity
	{
		std::string classname;
		math::Vec3 origin{};
		math::Vec3 angles{}; // pitch, yaw, roll (IW4 convention)
		bool has_origin = false;
		bool has_angles = false;
		std::unordered_map<std::string, std::string> kv;

		std::string get(const std::string& key, const std::string& fallback = "") const;
	};

	struct SpawnPoint
	{
		math::Vec3 origin{};
		float yaw_deg = 0.0f;
		float pitch_deg = 0.0f;
		std::string classname;
	};

	class EntityWorld
	{
	public:
		bool load_from_zone(const codol::zone::Zone& zone);
		bool parse_entity_string(std::string_view text);
		void clear();

		const std::vector<MapEntity>& entities() const { return entities_; }
		const std::vector<SpawnPoint>& player_spawns() const { return player_spawns_; }
		const std::vector<SpawnPoint>& intermissions() const { return intermissions_; }
		const std::vector<math::Vec3>& pathnodes() const { return pathnodes_; }

		SpawnPoint choose_initial_spawn(const math::AABB& fallback_bounds) const;

	private:
		std::vector<MapEntity> entities_;
		std::vector<SpawnPoint> player_spawns_;
		std::vector<SpawnPoint> intermissions_;
		std::vector<math::Vec3> pathnodes_;
	};
}
