#pragma once

// Project Dragon — Phase 4: Authentic Call of Duty / IW4 Player Movement (PM_Move)
// Standing/crouch/prone stances, walk/sprint/ADS speeds, jump, gravity, step-slide-move
// against real clipMap_t brush collision, and DualShock 4 camera control.

#include "../platform/platform.hpp"
#include "collision.hpp"
#include "math3d.hpp"

namespace dragon::world
{
	enum class Stance : std::uint8_t
	{
		Stand = 0,
		Crouch = 1,
		Prone = 2
	};

	struct PlayerState
	{
		math::Vec3 origin{ 0.0f, 0.0f, 64.0f };
		math::Vec3 velocity{ 0.0f, 0.0f, 0.0f };
		float yaw_deg = 0.0f;
		float pitch_deg = 0.0f;
		Stance stance = Stance::Stand;
		bool on_ground = false;
		bool sprinting = false;
		bool ads = false;
		bool noclip = false;
		float ads_fraction = 0.0f;
		float bob_phase = 0.0f;
		int health = 100;
		int max_health = 100;

		float eye_height() const;
		math::Vec3 eye_pos() const;
		math::Vec3 bbox_mins() const { return { -15.0f, -15.0f, 0.0f }; }
		math::Vec3 bbox_maxs() const;
	};

	class PlayerController
	{
	public:
		void spawn(const math::Vec3& origin, float yaw_deg, float pitch_deg, const CollisionWorld* coll);
		void update(const platform::InputState& input, float dt, const CollisionWorld* coll);

		PlayerState& state() { return ps_; }
		const PlayerState& state() const { return ps_; }

	private:
		void step_slide_move(float dt, const CollisionWorld& coll);
		void slide_move(float dt, const CollisionWorld& coll);

		PlayerState ps_;
	};
}
