#include "player_movement.hpp"

#include <algorithm>
#include <cmath>

namespace dragon::world
{
	namespace
	{
		constexpr float STEP_HEIGHT = 18.0f;    // IW4 PM_STEPSIZE
		constexpr float JUMP_VELOCITY = 250.0f; // IW4 jump impulse (~39u jump height at g=800)
		constexpr float OVERCLIP = 1.001f;

		math::Vec3 clip_velocity(const math::Vec3& in, const math::Vec3& normal)
		{
			float backoff = math::dot(in, normal);
			if (backoff < 0.0f) backoff *= OVERCLIP;
			else backoff /= OVERCLIP;
			return in - normal * backoff;
		}
	}

	float PlayerState::eye_height() const
	{
		switch (stance)
		{
		case Stance::Crouch: return 40.0f;
		case Stance::Prone:  return 11.0f;
		default:             return 60.0f;
		}
	}

	math::Vec3 PlayerState::bbox_maxs() const
	{
		switch (stance)
		{
		case Stance::Crouch: return { 15.0f, 15.0f, 50.0f };
		case Stance::Prone:  return { 15.0f, 15.0f, 30.0f };
		default:             return { 15.0f, 15.0f, 70.0f };
		}
	}

	math::Vec3 PlayerState::eye_pos() const
	{
		const float bob = (!noclip && on_ground) ? std::sin(bob_phase) * (sprinting ? 1.8f : 0.9f) : 0.0f;
		return { origin.x, origin.y, origin.z + eye_height() + bob };
	}

	void PlayerController::spawn(const math::Vec3& origin, float yaw_deg, float pitch_deg, const CollisionWorld* coll)
	{
		ps_ = {};
		ps_.origin = origin;
		ps_.yaw_deg = yaw_deg;
		ps_.pitch_deg = std::clamp(pitch_deg, -85.0f, 85.0f);

		// Drop gently onto floor brush if directly underneath (bounded to 64u so spawns on terrain meshes never plunge under the map)
		if (coll && coll->loaded())
		{
			const math::Vec3 start = origin + math::Vec3{ 0.0f, 0.0f, 12.0f };
			const math::Vec3 end = origin - math::Vec3{ 0.0f, 0.0f, 64.0f };
			const auto tr = coll->box_trace(start, end, ps_.bbox_mins(), ps_.bbox_maxs());
			if (!tr.start_solid && tr.fraction < 1.0f)
			{
				ps_.origin = tr.endpos + math::Vec3{ 0.0f, 0.0f, 1.0f };
				ps_.on_ground = true;
			}
		}
	}

	void PlayerController::update(const platform::InputState& input, float dt, const CollisionWorld* coll)
	{
		dt = std::clamp(dt, 0.0005f, 0.05f);

		// Sync noclip dvar
		if (input.pressed(platform::BTN_DPAD_UP))
		{
			ps_.noclip = !ps_.noclip;
			platform::DebugConsole::instance().set_dvar("g_noclip", ps_.noclip ? "1" : "0");
			platform::log_info("noclip %s", ps_.noclip ? "ON" : "OFF");
		}
		else
		{
			ps_.noclip = platform::DebugConsole::instance().dvar_bool("g_noclip", ps_.noclip);
		}

		// ADS smoothing (L2)
		ps_.ads = input.l2 > 0.35f || input.down(platform::BTN_L2);
		const float ads_target = ps_.ads ? 1.0f : 0.0f;
		ps_.ads_fraction += (ads_target - ps_.ads_fraction) * std::min(1.0f, dt * 14.0f);

		// Look sensitivity scales down while aiming down sights
		const float look_speed = ps_.ads ? 95.0f : 165.0f;
		ps_.yaw_deg -= input.rx * look_speed * dt;
		ps_.pitch_deg = std::clamp(ps_.pitch_deg + input.ry * (look_speed * 0.78f) * dt, -85.0f, 85.0f);
		if (ps_.yaw_deg > 360.0f) ps_.yaw_deg -= 360.0f;
		if (ps_.yaw_deg < 0.0f) ps_.yaw_deg += 360.0f;

		// Stance cycling (Circle)
		if (input.pressed(platform::BTN_CIRCLE))
		{
			if (ps_.stance == Stance::Stand) ps_.stance = Stance::Crouch;
			else if (ps_.stance == Stance::Crouch) ps_.stance = Stance::Prone;
			else ps_.stance = Stance::Stand;
		}
		if (input.pressed(platform::BTN_CROSS) && ps_.stance != Stance::Stand)
		{
			ps_.stance = Stance::Stand;
		}

		math::Vec3 fwd, right, up;
		math::angles_to_basis(ps_.yaw_deg, ps_.pitch_deg, fwd, right, up);

		const float base_speed = platform::DebugConsole::instance().dvar_float("g_speed", 190.0f);
		const float gravity = platform::DebugConsole::instance().dvar_float("g_gravity", 800.0f);

		if (ps_.noclip || !coll || !coll->loaded())
		{
			const float fly_speed = (input.down(platform::BTN_L3) || input.down(platform::BTN_L1)) ? 650.0f : 320.0f;
			math::Vec3 wish = fwd * input.ly + right * input.lx;
			if (input.down(platform::BTN_CROSS)) wish.z += 1.0f;
			if (input.down(platform::BTN_CIRCLE)) wish.z -= 1.0f;
			ps_.origin += wish * (fly_speed * dt);
			ps_.velocity = wish * fly_speed;
			ps_.on_ground = false;
			return;
		}

		// Ground check
		{
			const math::Vec3 probe_end = ps_.origin - math::Vec3{ 0.0f, 0.0f, 2.0f };
			const auto gtr = coll->box_trace(ps_.origin, probe_end, ps_.bbox_mins(), ps_.bbox_maxs());
			ps_.on_ground = (gtr.fraction < 1.0f && gtr.normal.z >= 0.7f);
		}

		// Horizontal movement basis
		math::Vec3 flat_fwd = math::normalize({ fwd.x, fwd.y, 0.0f });
		math::Vec3 flat_right = math::normalize({ right.x, right.y, 0.0f });
		math::Vec3 wish_dir = flat_fwd * input.ly + flat_right * input.lx;
		const float wish_mag = std::min(1.0f, math::length(wish_dir));
		if (wish_mag > 1e-4f) wish_dir = math::normalize(wish_dir);

		ps_.sprinting = (input.down(platform::BTN_L3) || input.down(platform::BTN_L1)) && input.ly > 0.5f && !ps_.ads && ps_.stance == Stance::Stand;
		float move_speed = base_speed;
		if (ps_.sprinting) move_speed *= 1.5f;
		else if (ps_.ads) move_speed *= 0.55f;
		else if (ps_.stance == Stance::Crouch) move_speed *= 0.65f;
		else if (ps_.stance == Stance::Prone) move_speed *= 0.35f;

		const float wish_speed = move_speed * wish_mag;

		if (ps_.on_ground)
		{
			// Ground friction (IW4 pm_friction = 5.5)
			const float speed = std::sqrt(ps_.velocity.x * ps_.velocity.x + ps_.velocity.y * ps_.velocity.y);
			if (speed > 0.1f)
			{
				const float drop = std::max(speed, 100.0f) * 5.5f * dt;
				const float new_speed = std::max(0.0f, speed - drop) / speed;
				ps_.velocity.x *= new_speed;
				ps_.velocity.y *= new_speed;
			}
			// Ground acceleration (IW4 pm_accelerate = 9.0)
			const float current = ps_.velocity.x * wish_dir.x + ps_.velocity.y * wish_dir.y;
			const float add_speed = wish_speed - current;
			if (add_speed > 0.0f)
			{
				const float accel = std::min(add_speed, 9.0f * wish_speed * dt);
				ps_.velocity.x += wish_dir.x * accel;
				ps_.velocity.y += wish_dir.y * accel;
			}
			ps_.velocity.z = 0.0f;

			if (input.pressed(platform::BTN_CROSS))
			{
				ps_.velocity.z = JUMP_VELOCITY;
				ps_.on_ground = false;
				platform::audio_play_tone(180.0f, 0.05f, 0.08f);
			}
		}
		else
		{
			// Air control + gravity
			const float current = ps_.velocity.x * wish_dir.x + ps_.velocity.y * wish_dir.y;
			const float add_speed = wish_speed - current;
			if (add_speed > 0.0f)
			{
				const float accel = std::min(add_speed, 1.5f * wish_speed * dt);
				ps_.velocity.x += wish_dir.x * accel;
				ps_.velocity.y += wish_dir.y * accel;
			}
			ps_.velocity.z -= gravity * dt;
		}

		step_slide_move(dt, *coll);

		const float horiz_speed = std::sqrt(ps_.velocity.x * ps_.velocity.x + ps_.velocity.y * ps_.velocity.y);
		if (ps_.on_ground && horiz_speed > 20.0f)
			ps_.bob_phase += dt * (ps_.sprinting ? 14.0f : 9.5f);
	}

	void PlayerController::slide_move(float dt, const CollisionWorld& coll)
	{
		float time_left = dt;
		math::Vec3 planes[5];
		int num_planes = 0;

		for (int bump = 0; bump < 4 && time_left > 1e-5f; ++bump)
		{
			const math::Vec3 end = ps_.origin + ps_.velocity * time_left;
			const auto tr = coll.box_trace(ps_.origin, end, ps_.bbox_mins(), ps_.bbox_maxs());
			if (tr.all_solid)
			{
				ps_.velocity = { 0, 0, 0 };
				return;
			}
			if (tr.fraction > 0.0f)
				ps_.origin = tr.endpos;
			if (tr.fraction >= 1.0f)
				break;

			time_left -= time_left * tr.fraction;
			if (num_planes < 5)
				planes[num_planes++] = tr.normal;

			ps_.velocity = clip_velocity(ps_.velocity, tr.normal);
			for (int i = 0; i < num_planes; ++i)
			{
				if (math::dot(ps_.velocity, planes[i]) < 0.0f)
					ps_.velocity = clip_velocity(ps_.velocity, planes[i]);
			}
		}
	}

	void PlayerController::step_slide_move(float dt, const CollisionWorld& coll)
	{
		const math::Vec3 start_o = ps_.origin;
		const math::Vec3 start_v = ps_.velocity;

		slide_move(dt, coll);

		const math::Vec3 down_o = ps_.origin;
		const math::Vec3 down_v = ps_.velocity;

		// Try stepping up up to STEP_HEIGHT units over curbs/stairs
		const math::Vec3 up_dest = start_o + math::Vec3{ 0.0f, 0.0f, STEP_HEIGHT };
		const auto up_tr = coll.box_trace(start_o, up_dest, ps_.bbox_mins(), ps_.bbox_maxs());
		if (up_tr.all_solid) return;

		ps_.origin = up_tr.endpos;
		ps_.velocity = start_v;
		slide_move(dt, coll);

		// Step back down to floor
		const math::Vec3 step_down = ps_.origin - math::Vec3{ 0.0f, 0.0f, STEP_HEIGHT };
		const auto dn_tr = coll.box_trace(ps_.origin, step_down, ps_.bbox_mins(), ps_.bbox_maxs());
		if (!dn_tr.all_solid) ps_.origin = dn_tr.endpos;

		const float down_dist = (down_o.x - start_o.x) * (down_o.x - start_o.x) + (down_o.y - start_o.y) * (down_o.y - start_o.y);
		const float step_dist = (ps_.origin.x - start_o.x) * (ps_.origin.x - start_o.x) + (ps_.origin.y - start_o.y) * (ps_.origin.y - start_o.y);
		if (down_dist >= step_dist)
		{
			ps_.origin = down_o;
			ps_.velocity = down_v;
		}
	}
}
