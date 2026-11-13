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

	enum class binding_kind : std::uint8_t { keyboard, mouse, gamepad_button, gamepad_axis };

	// One input an action is bound to. A module builds and reads it, so its layout is in the module layout key
	// (gs_module_layout.h); a new kind or field raises layout_revision, no major bump. `code` is the value of the enum
	// of the kind. `threshold` is the press point of a gamepad axis or trigger, 0 = the device's (gamepad_config).
	struct action_binding
	{
		binding_kind kind = binding_kind::keyboard;
		std::uint16_t code = 0;
		float threshold = 0.0f;

		constexpr action_binding() noexcept = default;
		constexpr action_binding(KEYBOARD_BUTTONS key) noexcept : kind(binding_kind::keyboard), code(static_cast<std::uint16_t>(key)) {}
		constexpr action_binding(MOUSE_BUTTONS button) noexcept : kind(binding_kind::mouse), code(static_cast<std::uint16_t>(button)) {}
		constexpr action_binding(GAMEPAD_BUTTONS button, float press_threshold = 0.0f) noexcept
			: kind(binding_kind::gamepad_button), code(static_cast<std::uint16_t>(button)), threshold(press_threshold) {}
		constexpr action_binding(GAMEPAD_AXES axis, float press_threshold = 0.0f) noexcept
			: kind(binding_kind::gamepad_axis), code(static_cast<std::uint16_t>(axis)), threshold(press_threshold) {}

		constexpr bool is_gamepad() const noexcept { return kind == binding_kind::gamepad_button || kind == binding_kind::gamepad_axis; }
		constexpr std::optional<KEYBOARD_BUTTONS> key() const noexcept
		{
			return kind == binding_kind::keyboard ? std::optional{ static_cast<KEYBOARD_BUTTONS>(code) } : std::nullopt;
		}
		constexpr std::optional<MOUSE_BUTTONS> mouse_button() const noexcept
		{
			return kind == binding_kind::mouse ? std::optional{ static_cast<MOUSE_BUTTONS>(code) } : std::nullopt;
		}
		constexpr std::optional<GAMEPAD_BUTTONS> gamepad_button() const noexcept
		{
			return kind == binding_kind::gamepad_button ? std::optional{ static_cast<GAMEPAD_BUTTONS>(code) } : std::nullopt;
		}
		constexpr std::optional<GAMEPAD_AXES> gamepad_axis() const noexcept
		{
			return kind == binding_kind::gamepad_axis ? std::optional{ static_cast<GAMEPAD_AXES>(code) } : std::nullopt;
		}

		bool operator==(const action_binding&) const noexcept = default;
	};
	static_assert(std::is_trivially_copyable_v<action_binding>);

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
