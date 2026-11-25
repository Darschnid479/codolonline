#pragma once

// Project Dragon — Phase 4: Engine Reconstruction Gameplay Subsystems
//   1. WeaponSystem: reads real ASSET_TYPE_WEAPON asset headers from CODOL zones,
//      state machine (idle/fire/ADS/reload/switch), raycast hit detection via CollisionWorld
//   2. SkeletalAnimator: evaluates XModel bone hierarchies & XAnimParts frame timing
//   3. GscRuntime: clean-room GSC script loader, parser, and cooperative thread VM
//      (loads real .gsc files from FastFile RawFile assets and main/sh1x_svr_scripts.iwd)
//   4. OfflineServer: local offline match session (TDM/FFA/Zombies/Cyborg survival),
//      waypoint bot navigation using real MapEnts node_pathnodes

#include "../assets/asset_manager.hpp"
#include "../platform/platform.hpp"
#include "collision.hpp"
#include "entities.hpp"
#include "math3d.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dragon::world
{
	// ---- Weapons ------------------------------------------------------------
	struct WeaponInfo
	{
		std::string internal_name;
		std::string display_name;
		int clip_size = 30;
		int ammo_in_clip = 30;
		int ammo_reserve = 120;
		float fire_interval_sec = 0.095f;
		float reload_time_sec = 2.0f;
		float ads_fov = 55.0f;
		int damage = 35;
	};

	struct WeaponImpact
	{
		math::Vec3 pos{};
		math::Vec3 normal{ 0, 0, 1 };
		float ttl = 0.0f;
	};

	class WeaponSystem
	{
	public:
		void load_from_zone(const codol::zone::Zone& zone);
		void update(const platform::InputState& input, float dt, const math::Vec3& eye, const math::Vec3& forward,
			const CollisionWorld* coll, std::vector<struct BotEntity>* bots = nullptr);

		const WeaponInfo& active_weapon() const;
		const std::vector<WeaponInfo>& inventory() const { return weapons_; }
		const std::vector<WeaponImpact>& recent_impacts() const { return impacts_; }
		bool is_reloading() const { return reload_timer_ > 0.0f; }
		float recoil_pitch() const { return recoil_pitch_; }
		std::uint32_t shots_fired() const { return shots_fired_; }
		std::uint32_t hits_registered() const { return hits_registered_; }

	private:
		std::vector<WeaponInfo> weapons_;
		std::size_t active_idx_ = 0;
		float fire_cooldown_ = 0.0f;
		float reload_timer_ = 0.0f;
		float recoil_pitch_ = 0.0f;
		std::uint32_t shots_fired_ = 0;
		std::uint32_t hits_registered_ = 0;
		std::vector<WeaponImpact> impacts_;
	};

	// ---- Skeletal Animation (XModel + XAnimParts) ---------------------------
	struct SkeletonPose
	{
		std::string model_name;
		std::string anim_name;
		std::vector<std::string> bone_names;
		std::vector<int> parent_indices;
		std::vector<math::Vec3> world_positions;
		float time_sec = 0.0f;
		float framerate = 30.0f;
		std::uint32_t frame_count = 1;
	};

	class SkeletalRegistry
	{
	public:
		void index_zone(const codol::zone::Zone& zone);
		std::size_t anim_count() const { return anim_names_.size(); }
		std::size_t rigged_model_count() const { return rigged_models_.size(); }
		bool evaluate_bind_pose(const codol::zone::Zone& zone, const std::string& model_name, SkeletonPose& out) const;

	private:
		std::vector<std::string> anim_names_;
		std::vector<std::string> rigged_models_;
	};

	// ---- Clean-Room GSC Runtime VM ------------------------------------------
	struct GscFunction
	{
		std::string name;
		std::vector<std::string> statements;
	};

	struct GscThread
	{
		std::string script_name;
		std::string func_name;
		std::size_t pc = 0;
		float wait_timer = 0.0f;
		std::string waiting_notify;
		bool finished = false;
	};

	class GscRuntime
	{
	public:
		void reset();
		bool load_script_source(const std::string& script_name, std::string_view source);
		std::size_t load_from_zone_and_assets(const codol::zone::Zone& zone, const assets::AssetManager& mgr, const std::string& map_name);

		void spawn_thread(const std::string& script_name, const std::string& func_name);
		void notify(const std::string& event_name);
		void tick(float dt);

		std::size_t loaded_script_count() const { return scripts_.size(); }
		std::size_t function_count() const;
		std::size_t active_thread_count() const;
		std::uint64_t executed_statements() const { return executed_statements_; }
		const std::unordered_map<std::string, std::string>& level_vars() const { return level_vars_; }

	private:
		void exec_statement(GscThread& th, const std::string& stmt);

		std::unordered_map<std::string, std::vector<GscFunction>> scripts_;
		std::vector<GscThread> threads_;
		std::unordered_map<std::string, std::string> level_vars_;
		std::uint64_t executed_statements_ = 0;
	};

	// ---- Offline Match Server & Waypoint Bot AI -----------------------------
	struct BotEntity
	{
		int id = 0;
		std::string name;
		int team = 1; // 0 = Allies, 1 = Axis / Zombies / Cyborg
		math::Vec3 origin{};
		float yaw_deg = 0.0f;
		int health = 100;
		int max_health = 100;
		bool alive = true;
		float respawn_timer = 0.0f;
		std::size_t target_node = 0;
	};

	class OfflineServer
	{
	public:
		void start_match(const std::string& map_name, const std::string& gametype,
			const EntityWorld& ents, const codol::zone::Zone& zone, const assets::AssetManager& mgr);
		void tick(float dt, const math::Vec3& player_pos, const CollisionWorld* coll);

		const std::string& gametype() const { return gametype_; }
		int wave_number() const { return wave_number_; }
		int player_score() const { return player_score_; }
		int player_kills() const { return player_kills_; }
		std::vector<BotEntity>& bots() { return bots_; }
		const std::vector<BotEntity>& bots() const { return bots_; }
		GscRuntime& gsc() { return gsc_; }
		const GscRuntime& gsc() const { return gsc_; }

	private:
		std::string map_name_;
		std::string gametype_ = "war";
		int wave_number_ = 1;
		int player_score_ = 0;
		int player_kills_ = 0;
		float server_accum_ = 0.0f;
		std::vector<math::Vec3> waypoints_;
		std::vector<SpawnPoint> spawns_;
		std::vector<BotEntity> bots_;
		GscRuntime gsc_;
	};
}
