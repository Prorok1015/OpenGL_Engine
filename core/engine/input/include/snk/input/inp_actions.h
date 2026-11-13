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
#include <variant>
#include <vector>

// Game actions (WORLD_BUS.md 4): named actions and the keys, mouse buttons and gamepad buttons and axes bound to them. The map is built once from
// res/input/actions.desc (scn::build_action_map) and shared read-only by the worlds; an action is held while any of its
// bindings is down.
namespace inp
{
	using action_id = std::uint32_t;
	inline constexpr action_id invalid_action = std::numeric_limits<action_id>::max();

	// Alternatives are appended only: modules built before the gamepad ones (SNK_MODULE_API_VERSION 3.16) read this array
	// by the size and the indices of the keyboard and mouse. One byte of value keeps it two bytes; a threshold does not fit,
	// so it lives in gamepad_config.
	using action_binding = std::variant<KEYBOARD_BUTTONS, MOUSE_BUTTONS, GAMEPAD_BUTTONS, GAMEPAD_AXES>;
	static_assert(sizeof(action_binding) == 2 && alignof(action_binding) == 1,
		"action_binding keeps two bytes: modules read the bindings of an action by its size");
	static_assert(std::is_same_v<std::variant_alternative_t<0, action_binding>, KEYBOARD_BUTTONS>
		&& std::is_same_v<std::variant_alternative_t<1, action_binding>, MOUSE_BUTTONS>,
		"the keyboard and mouse alternatives keep their indices (0, 1)");

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
		std::span<const action> actions() const { return m_actions; }
		std::size_t size() const { return m_actions.size(); }

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
