#pragma once
#include <snk/input/inp_events.hpp>
#include <snk/common/snk_api.h>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Game actions (WORLD_BUS.md 4): named actions and the keys, mouse buttons and gamepad buttons and axes bound to them. The map is built once from
// res/input/actions.desc (scn::build_action_map) and shared read-only by the worlds; an action is held while any of its
// bindings is down.
namespace inp
{
	using action_id = std::uint32_t;
	inline constexpr action_id invalid_action = std::numeric_limits<action_id>::max();

	// `none` is what a default-made binding is: it never reads as a key.
	enum class binding_kind : std::uint8_t { none, keyboard, mouse, gamepad_button, gamepad_axis };

	// One input an action is bound to. A module builds and reads it, so its layout is in the module layout key
	// (gs_module_layout.h); a new kind or field raises layout_revision, no major bump. It is made only from the enum of a
	// kind, so `code` is always a value of that enum. `threshold` is the press point of a gamepad button or axis in 0..1 (an
	// out-of-range or NaN value becomes 0), 0 = the device's (gamepad_config).
	class action_binding
	{
	public:
		constexpr action_binding() noexcept = default;
		constexpr action_binding(KEYBOARD_BUTTONS key) noexcept : m_kind(binding_kind::keyboard), m_code(static_cast<std::uint16_t>(key)) {}
		constexpr action_binding(MOUSE_BUTTONS button) noexcept : m_kind(binding_kind::mouse), m_code(static_cast<std::uint16_t>(button)) {}
		constexpr action_binding(GAMEPAD_BUTTONS button, float press_threshold = 0.0f) noexcept
			: m_kind(binding_kind::gamepad_button), m_code(static_cast<std::uint16_t>(button)), m_threshold(valid_threshold(press_threshold)) {}
		constexpr action_binding(GAMEPAD_AXES axis, float press_threshold = 0.0f) noexcept
			: m_kind(binding_kind::gamepad_axis), m_code(static_cast<std::uint16_t>(axis)), m_threshold(valid_threshold(press_threshold)) {}

		constexpr binding_kind kind() const noexcept { return m_kind; }
		constexpr std::uint16_t code() const noexcept { return m_code; }
		constexpr float threshold() const noexcept { return m_threshold; }
		constexpr bool is_gamepad() const noexcept { return m_kind == binding_kind::gamepad_button || m_kind == binding_kind::gamepad_axis; }
		constexpr std::optional<KEYBOARD_BUTTONS> key() const noexcept
		{
			return m_kind == binding_kind::keyboard ? std::optional{ static_cast<KEYBOARD_BUTTONS>(m_code) } : std::nullopt;
		}
		constexpr std::optional<MOUSE_BUTTONS> mouse_button() const noexcept
		{
			return m_kind == binding_kind::mouse ? std::optional{ static_cast<MOUSE_BUTTONS>(m_code) } : std::nullopt;
		}
		constexpr std::optional<GAMEPAD_BUTTONS> gamepad_button() const noexcept
		{
			return m_kind == binding_kind::gamepad_button ? std::optional{ static_cast<GAMEPAD_BUTTONS>(m_code) } : std::nullopt;
		}
		constexpr std::optional<GAMEPAD_AXES> gamepad_axis() const noexcept
		{
			return m_kind == binding_kind::gamepad_axis ? std::optional{ static_cast<GAMEPAD_AXES>(m_code) } : std::nullopt;
		}

		// The same key, button or axis, whatever the thresholds: what makes two bindings a conflict or a duplicate.
		// operator== compares the thresholds too (equal values).
		constexpr bool same_input(const action_binding& other) const noexcept { return m_kind == other.m_kind && m_code == other.m_code; }
		bool operator==(const action_binding&) const noexcept = default;

	private:
		static constexpr float valid_threshold(float value) noexcept { return value >= 0.0f && value <= 1.0f ? value : 0.0f; }

		binding_kind m_kind = binding_kind::none;
		std::uint16_t m_code = 0;
		float m_threshold = 0.0f;
	};
	static_assert(sizeof(action_binding) == 8 && alignof(action_binding) == 4 && std::is_trivially_copyable_v<action_binding>,
		"action_binding is built and read by modules: its layout is part of the module layout key");

	struct action
	{
		std::string name;
		std::vector<action_binding> bindings;
	};

	class SNK_API action_map
	{
	public:
		// Ids follow the order of add(). False, and nothing added, for an empty name or one already there.
		bool add(std::string name, std::vector<action_binding> bindings);

		// Look an action up once (at init, on the main thread: the warned names are kept without a lock) and keep its
		// id. An unknown name gives invalid_action and a warning, once per name.
		action_id id(std::string_view name) const;
		// Null for invalid_action or an id out of range.
		const action* find(action_id id) const;
		// Out of line: a module reads the map through these, never by the offset of m_actions.
		std::span<const action> actions() const;
		std::size_t size() const;

	private:
		std::vector<action> m_actions;
		mutable std::set<std::string, std::less<>> m_warned;
	};

	// "A", "LB", "DPAD_UP", "LSTICK": the names of GAMEPAD_BUTTONS, as written in actions.desc.
	SNK_API std::optional<GAMEPAD_BUTTONS> gamepad_button_from_name(std::string_view name);
	// Empty for COUNT.
	SNK_API std::string_view gamepad_button_name(GAMEPAD_BUTTONS button);
	// "LeftX-", "LeftX+", "LeftY-", "LeftY+", "RightX-" ... "RightY+", "LeftTrigger", "RightTrigger": a directed axis.
	SNK_API std::optional<GAMEPAD_AXES> gamepad_axis_from_name(std::string_view name);
	// Empty for COUNT.
	SNK_API std::string_view gamepad_axis_name(GAMEPAD_AXES axis);
	// An axis of a device and the direction a directed axis is pressed in: +1 or -1.
	SNK_API GAMEPAD_AXIS_IDS gamepad_axis_id(GAMEPAD_AXES axis);
	SNK_API int gamepad_axis_direction(GAMEPAD_AXES axis);
}
