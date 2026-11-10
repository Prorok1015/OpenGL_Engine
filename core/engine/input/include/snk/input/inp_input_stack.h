#pragma once
#include <snk/input/inp_actions.h>
#include <snk/input/inp_events.hpp>
#include <snk/common/snk_api.h>
#include <glm/glm.hpp>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The input stack of a set of worlds (INPUT_STACK.md): layers that take keys and mouse units for themselves and hide
// them from the layers below. Pure data and functions, no scene types: the worlds are known by name.
namespace inp
{
	enum class layer_owner : std::uint8_t { engine, editor, module };

	inline constexpr std::size_t key_count = static_cast<std::size_t>(KEYBOARD_BUTTONS::COUNT);

	struct stack_layer_desc
	{
		// Unique within a world.
		std::string name;
		// Higher is on top.
		int order = 0;
		// An inactive layer receives nothing and takes nothing.
		bool active = true;
		// Taken by the layer: the keyboard keys of these actions (#624; the mouse buttons of an action follow the layer's
		// click rules), these keys, the printable keys. The layer also declares the actions: the world gets one when a
		// binding of it reached this layer (stack_resolution, world_input::actions).
		std::vector<std::string> actions;
		std::vector<KEYBOARD_BUTTONS> keys;
		bool text_keys = false;
		// Every key; what a focused text field of a UI world took before layers existed.
		bool whole_keyboard = false;
		// Clicks and wheel inside these rects {x0, y0, x1, y1}, bounds included, are the layer's.
		std::vector<glm::ivec4> regions;
		bool block_below = false;
		// Seen by the layer but passed on (not used yet).
		std::vector<std::string> listen;
		layer_owner owner = layer_owner::engine;
		// Declares every action of the map given to resolve, besides `actions` (the default reader of a set, INPUT_STACK.md 6).
		bool all_actions = false;
	};

	struct stack_entry
	{
		std::string world;
		std::uint32_t desc_index = 0;
		stack_layer_desc layer;
		std::uint64_t added = 0;
	};

	class SNK_API input_stack
	{
	public:
		// A layer of the world with the same name is replaced and keeps its place of adding.
		void put(std::string_view world, std::uint32_t desc_index, stack_layer_desc layer);
		// The worlds that are not in `worlds` lose their layers.
		void retain_worlds(const std::vector<std::string>& worlds);
		void remove_world(std::string_view world);
		const stack_layer_desc* find(std::string_view world, std::string_view name) const;
		std::size_t size() const { return m_entries.size(); }

		// Top first: by `order` (higher first), then by the world's desc_index, then the later added layer first.
		std::vector<const stack_entry*> ordered() const;

	private:
		std::vector<stack_entry> m_entries;
		std::uint64_t m_added = 0;
	};

	// A mouse button held since a press: `captured` when a layer took the press, then `world` is its world.
	struct drag_hold
	{
		bool active = false;
		bool captured = false;
		std::string world;
		// The layer that took the press; empty: any layer of `world` (what the mouse bindings of actions reach).
		std::string layer{};
	};

	// What of one action reaches the layers of a world that declare it (INPUT_STACK.md 3.2): the keyboard keys that no layer
	// above took, and whether the world got the mouse buttons. The action is down when a binding is down and reached.
	struct action_reach
	{
		std::bitset<key_count> keys;
		bool buttons = false;
	};

	// What reaches a world after the stack has been walked.
	struct world_input
	{
		// A key that is false here is hidden from the world.
		std::array<bool, key_count> keys{};
		// Buttons and wheel.
		bool buttons = false;
		bool motion = false;
		// Gates the mouse-driven camera: the world got buttons, or got motion while no button is down and the wheel is still.
		bool mouse_focus = false;
		// A layer that takes the keyboard got keys.
		bool keyboard_focus = false;
		// By action id of the map given to resolve; empty without one. An action no layer of the world declares has no keys.
		std::vector<action_reach> actions;
	};

	struct stack_resolution
	{
		std::map<std::string, world_input, std::less<>> worlds;

		const world_input* find(std::string_view world) const
		{
			const auto it = worlds.find(world);
			return it == worlds.end() ? nullptr : &it->second;
		}
		world_input* find(std::string_view world)
		{
			const auto it = worlds.find(world);
			return it == worlds.end() ? nullptr : &it->second;
		}
	};

	// The world of the top layer that takes a click at `cursor`: a layer with `block_below` or one whose regions hold it.
	// Only layers of the `fed` worlds count.
	SNK_API std::optional<std::string> mouse_taker(const input_stack& stack, const std::vector<std::string>& fed, glm::vec2 cursor);
	// The same layer as an entry (its world and name); null when none takes it.
	SNK_API const stack_entry* mouse_taker_entry(const input_stack& stack, const std::vector<std::string>& fed, glm::vec2 cursor);

	// `engaged`: a button is down or the wheel moved this frame. `actions`: the map that gives the names a layer declares their
	// bindings (#624); a layer takes the keyboard keys of its actions like its `keys`, and the world gets an action when
	// a binding of it reaches a layer that declares it: A taken above and the arrow key not, the action is still there (OR).
	// A mouse binding reaches the layers that get the click or the drag: the layer it lands on and the ones above it (the
	// ones of the same world, or all above when it blocks); a modal layer of a world hides the mouse actions of the layers below it.
	SNK_API stack_resolution resolve(const input_stack& stack, const std::vector<std::string>& fed, glm::vec2 cursor, bool engaged, const drag_hold& hold,
		const action_map* actions = nullptr);

	// The printable keys of INPUT_STACK.md §4.4; Escape, Enter, Tab, F-keys and modifiers are not.
	SNK_API bool is_text_key(KEYBOARD_BUTTONS key);
	// "W", "K_0", "ESCAPE": the names of KEYBOARD_BUTTONS (physical positions on a US layout, INPUT_STACK.md).
	SNK_API std::optional<KEYBOARD_BUTTONS> key_from_name(std::string_view name);
	// The name key_from_name reads; empty for COUNT.
	SNK_API std::string_view key_code_name(KEYBOARD_BUTTONS key);
	// "LEFT", "RIGHT", "MIDDLE", "BTN_4".."BTN_8": one name per button, the aliases BTN_1..BTN_3 and LAST have none.
	SNK_API std::optional<MOUSE_BUTTONS> mouse_button_from_name(std::string_view name);
	// Empty for LAST and COUNT.
	SNK_API std::string_view mouse_button_name(MOUSE_BUTTONS button);
}
