#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// The gamepad as the engine sees it, whatever device is behind it (Xbox layout, as GLFW's gamepad mapping). Values index
// arrays and are appended-only (new entries before COUNT).
namespace core::inp
{
	enum class GAMEPAD_BUTTONS : uint8_t
	{
		A, B, X, Y,
		LB, RB,
		BACK, START, GUIDE,
		LSTICK, RSTICK,
		DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
		COUNT
	};

	// The axes as a device reports them, -1..1 (triggers 0..1).
	enum class GAMEPAD_AXIS_IDS : uint8_t
	{
		LEFT_X, LEFT_Y, RIGHT_X, RIGHT_Y, LEFT_TRIGGER, RIGHT_TRIGGER,
		COUNT
	};

	// An axis taken as a button, with its direction: one value, so a binding stays one byte (WORLD_BUS.md 4.6). Y is
	// positive downwards, as the devices report it.
	enum class GAMEPAD_AXES : uint8_t
	{
		LEFT_X_NEG, LEFT_X_POS, LEFT_Y_NEG, LEFT_Y_POS,
		RIGHT_X_NEG, RIGHT_X_POS, RIGHT_Y_NEG, RIGHT_Y_POS,
		LEFT_TRIGGER, RIGHT_TRIGGER,
		COUNT
	};

	inline constexpr std::size_t gamepad_button_count = static_cast<std::size_t>(GAMEPAD_BUTTONS::COUNT);
	inline constexpr std::size_t gamepad_axis_count = static_cast<std::size_t>(GAMEPAD_AXIS_IDS::COUNT);

	// What one poll of a device gives.
	struct gamepad_state
	{
		bool connected = false;
		std::array<bool, gamepad_button_count> buttons{};
		std::array<float, gamepad_axis_count> axes{};
	};

	// Shared by the bindings of a device: below `deadzone` an axis reads 0, and a directed axis counts as pressed past
	// `press_threshold`. Per device, not per binding, so a binding stays one byte.
	struct gamepad_config
	{
		float deadzone = 0.25f;
		float press_threshold = 0.5f;
		bool operator==(const gamepad_config&) const = default;
	};
}
