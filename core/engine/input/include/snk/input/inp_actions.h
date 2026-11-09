#pragma once
#include <snk/input/inp_events.hpp>
#include <snk/common/snk_api.h>
#include <cstdint>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Game actions (WORLD_BUS.md 4): named actions and the keys and mouse buttons bound to them. The map is built once from
// res/input/actions.desc (scn::build_action_map) and shared read-only by the worlds; an action is held while any of its
// bindings is down.
namespace inp
{
	using action_id = std::uint32_t;
	inline constexpr action_id invalid_action = std::numeric_limits<action_id>::max();

	using action_binding = std::variant<KEYBOARD_BUTTONS, MOUSE_BUTTONS>;

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

		// Look an action up once and keep its id. An unknown name gives invalid_action and a warning, once per name.
		action_id id(std::string_view name) const;
		// Null for invalid_action or an id out of range.
		const action* find(action_id id) const;
		std::span<const action> actions() const { return m_actions; }
		std::size_t size() const { return m_actions.size(); }

	private:
		std::vector<action> m_actions;
		mutable std::set<std::string, std::less<>> m_warned;
	};
}
