#if defined(DRAGON_PS4) || defined(__PS4__) || defined(__ORBIS__)

#include "platform.hpp"

#include <orbis/Pad.h>
#include <orbis/UserService.h>

#include <cmath>
#include <cstring>

namespace dragon::platform
{
	namespace
	{
		int32_t g_pad_handle = -1;
		int32_t g_user_id = ORBIS_USER_SERVICE_USER_ID_INVALID;

		float normalize_stick(std::uint8_t raw, float deadzone = 0.14f)
		{
			const float v = (static_cast<float>(raw) - 127.5f) / 127.5f;
			const float a = std::fabs(v);
			if (a < deadzone) return 0.0f;
			const float scaled = (a - deadzone) / (1.0f - deadzone);
			return v < 0.0f ? -scaled : scaled;
		}
	}

	bool input_init()
	{
		if (scePadInit() != 0)
		{
			log_warn("scePadInit failed");
			return false;
		}
		if (sceUserServiceGetInitialUser(&g_user_id) != 0)
		{
			g_user_id = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
		}
		g_pad_handle = scePadOpen(g_user_id, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
		if (g_pad_handle < 0)
		{
			log_warn("scePadOpen failed (0x%08X)", static_cast<unsigned>(g_pad_handle));
			return false;
		}
		// Set Project Dragon signature amber-orange DualShock 4 lightbar
		input_set_lightbar(255, 110, 20);
		log_info("DualShock 4 initialized (handle=%d, user=%d)", g_pad_handle, g_user_id);
		return true;
	}

	void input_shutdown()
	{
		if (g_pad_handle >= 0)
		{
			scePadResetLightBar(g_pad_handle);
			scePadClose(g_pad_handle);
			g_pad_handle = -1;
		}
	}

	void input_poll(InputState& state)
	{
		state.prev_buttons = state.buttons;
		state.buttons = 0;
		state.lx = state.ly = state.rx = state.ry = 0.0f;
		state.l2 = state.r2 = 0.0f;
		state.connected = false;

		if (g_pad_handle < 0) return;

		OrbisPadData pad{};
		if (scePadReadState(g_pad_handle, &pad) != 0)
			return;

		state.connected = pad.connected != 0;
		state.lx = normalize_stick(pad.leftStick.x);
		state.ly = -normalize_stick(pad.leftStick.y); // +1 forward/up
		state.rx = normalize_stick(pad.rightStick.x);
		state.ry = -normalize_stick(pad.rightStick.y); // +1 look up
		state.l2 = static_cast<float>(pad.analogButtons.l2) / 255.0f;
		state.r2 = static_cast<float>(pad.analogButtons.r2) / 255.0f;

		const std::uint32_t b = pad.buttons;
		if (b & ORBIS_PAD_BUTTON_CROSS)     state.buttons |= BTN_CROSS;
		if (b & ORBIS_PAD_BUTTON_CIRCLE)    state.buttons |= BTN_CIRCLE;
		if (b & ORBIS_PAD_BUTTON_SQUARE)    state.buttons |= BTN_SQUARE;
		if (b & ORBIS_PAD_BUTTON_TRIANGLE)  state.buttons |= BTN_TRIANGLE;
		if (b & ORBIS_PAD_BUTTON_L1)        state.buttons |= BTN_L1;
		if (b & ORBIS_PAD_BUTTON_R1)        state.buttons |= BTN_R1;
		if (b & ORBIS_PAD_BUTTON_L2)        state.buttons |= BTN_L2;
		if (b & ORBIS_PAD_BUTTON_R2)        state.buttons |= BTN_R2;
		if (b & ORBIS_PAD_BUTTON_L3)        state.buttons |= BTN_L3;
		if (b & ORBIS_PAD_BUTTON_R3)        state.buttons |= BTN_R3;
		if (b & ORBIS_PAD_BUTTON_OPTIONS)   state.buttons |= BTN_OPTIONS;
		if (b & ORBIS_PAD_BUTTON_TOUCH_PAD) state.buttons |= BTN_TOUCHPAD;
		if (b & ORBIS_PAD_BUTTON_UP)        state.buttons |= BTN_DPAD_UP;
		if (b & ORBIS_PAD_BUTTON_DOWN)      state.buttons |= BTN_DPAD_DOWN;
		if (b & ORBIS_PAD_BUTTON_LEFT)      state.buttons |= BTN_DPAD_LEFT;
		if (b & ORBIS_PAD_BUTTON_RIGHT)     state.buttons |= BTN_DPAD_RIGHT;
	}

	void input_set_lightbar(std::uint8_t r, std::uint8_t g, std::uint8_t b)
	{
		if (g_pad_handle < 0) return;
		OrbisPadColor c{ r, g, b, 255 };
		scePadSetLightBar(g_pad_handle, &c);
	}

	void input_set_rumble(std::uint8_t small_motor, std::uint8_t large_motor)
	{
		if (g_pad_handle < 0) return;
		OrbisPadVibeParam vibe{ large_motor, small_motor };
		scePadSetVibration(g_pad_handle, &vibe);
	}
}

#endif
