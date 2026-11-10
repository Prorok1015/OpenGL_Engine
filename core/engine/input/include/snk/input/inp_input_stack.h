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
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The input stack of a set of worlds (INPUT_STACK.md): layers that take keys and mouse units for themselves and hide
// them from the layers below. Pure data and functions, no scene types: the worlds are known by name.
namespace inp
{
	enum class layer_owner : std::uint8_t { engine, editor, module };

	inline constexpr std::size_t key_count = static_cast<std::size_t>(KEYBOARD_BUTTONS::COUNT);
	inline constexpr std::size_t mouse_count = static_cast<std::size_t>(MOUSE_BUTTONS::COUNT);

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

	// What game code asks of the stack (INPUT_STACK.md 3.3). Plain data: it waits for the next gs::route_input, which applies it
	// with apply_commands. `source` is who asks: it owns the layers a push makes, and only its own layers can be changed.
	enum class stack_command_kind : std::uint8_t { push, replace, remove, set_active, set_order, set_world_active };

	struct stack_command
	{
		stack_command_kind kind = stack_command_kind::push;
		// The world of the layer; for set_world_active, the world to switch. Empty from a system: its own world.
		std::string world{};
		// The layer for remove, set_active and set_order; push and replace name theirs in `layer.name`.
		std::string name{};
		stack_layer_desc layer{};
		int order = 0;
		bool active = true;
		layer_owner source = layer_owner::module;
	};

	// The ctx resource a system of a world uses to ask for changes of the stack of its set: it fills a list that gs::route_input
	// takes at the start of the next frame (worlds in the order of their desc_index, calls in order). The commands are about
	// the layers of the world of the system; set_world_active names any world of the set.
	class SNK_API input_stack_control
	{
	public:
		// A layer of the system's world; one with the same name is replaced (a repeated setup makes no second layer).
		void push(stack_layer_desc layer);
		// Like push, but the layer must exist already.
		void replace(stack_layer_desc layer);
		void remove(std::string name);
		void set_active(std::string name, bool active);
		void set_order(std::string name, int order);
		// Every layer of the world, of any owner, switched on or off (the base layer too).
		void set_world_active(std::string world, bool active);

		bool empty() const { return m_commands.empty(); }
		// What gs::route_input takes: the commands in call order; the list is emptied.
		std::vector<stack_command> take();

	private:
		std::vector<stack_command> m_commands;
	};

	struct stack_entry
	{
		std::string world;
		std::uint32_t desc_index = 0;
		stack_layer_desc layer;
		std::uint64_t added = 0;
	};

	// The memory of fronts of one layer, outside the worlds (INPUT_STACK.md 5): the keys and buttons that may give a press
	// next frame (armed), as plain data. A layer with no memory starts with every key held right now disarmed ("previous =
	// current"): Escape that opened a menu does not close it. Dropped with the layer.
	struct layer_memory
	{
		bool initialized = false;
		std::bitset<key_count> armed_keys;
		std::bitset<mouse_count> armed_buttons;
	};

	struct world_ref
	{
		std::string name;
		std::uint32_t desc_index = 0;
	};

	// A stack as a hot reload of the module keeps it (HOT_RELOAD_PLAY.md 4.4): the layers that are not the engine's (it puts
	// the base layer of every world each frame) in the order they were added, and the worlds that are off, as plain data that
	// outlives the code that pushed them. The memory of fronts is not kept: a restored layer starts with "previous = current".
	struct stack_snapshot
	{
		struct layer
		{
			std::string world;
			stack_layer_desc desc;
		};
		std::vector<layer> layers;
		std::vector<std::string> off_worlds;
	};

	struct stack_restore_report
	{
		std::size_t restored = 0;
		// Layers of a world that is not in the new run: left out, not an error.
		std::size_t skipped = 0;
	};

	class SNK_API input_stack;
	struct stack_resolution;
	SNK_API void arm_actions(input_stack& stack, stack_resolution& resolved, const input_state& physical, const action_map* actions);

	class SNK_API input_stack
	{
	public:
		// A layer of the world with the same name is replaced and keeps its place of adding. The same layer put again (the base
		// layer of every world, every frame) keeps its memory of fronts; one put with another desc_index is a new world's and starts
		// again.
		void put(std::string_view world, std::uint32_t desc_index, stack_layer_desc layer);
		// The worlds that are not in `worlds` lose their layers.
		void retain_worlds(const std::vector<std::string>& worlds);
		void remove_world(std::string_view world);
		const stack_layer_desc* find(std::string_view world, std::string_view name) const;
		std::size_t size() const { return m_entries.size(); }
		// Takes the layer away with its memory of fronts; false when there is none.
		bool remove(std::string_view world, std::string_view name);
		// Every layer of the owner (a module's layers go when it unloads or reloads), memories included.
		void remove_owner(layer_owner owner);
		// A world that is off has all its layers inactive, whatever their own flag says (set_world_active); it is on by default
		// and a world that is made again is on. Dropped with the world.
		void set_world_active(std::string_view world, bool active);
		bool world_active(std::string_view world) const;
		// To change a layer in place (apply_commands); null when there is none.
		stack_layer_desc* find(std::string_view world, std::string_view name);

		// The memory of fronts of a layer (arm_actions writes it); dropped with the layer, its world or the set, and when the
		// world's desc_index changes (put: the world was made again).
		const layer_memory* find_memory(std::string_view world, std::string_view layer) const;
		// True the first time `key` is asked for in this stack: a refused command is said once per session of the set (the stack
		// goes with the set), not every frame it is asked again.
		bool warn_once(std::string key);
		// What a hot reload keeps of the stack, and putting it back into the stack of the new run: a layer of a world that is in
		// `worlds` is put with that world's desc_index (the same name replaces, so doing it twice makes no second layer); the
		// worlds that were off are switched off again.
		stack_snapshot snapshot() const;
		stack_restore_report restore(const stack_snapshot& saved, const std::vector<world_ref>& worlds);
		std::size_t memory_count() const { return m_memory.size(); }

		// Top first: by `order` (higher first), then by the world's desc_index, then the later added layer first.
		std::vector<const stack_entry*> ordered() const;

	private:
		friend SNK_API void arm_actions(input_stack& stack, stack_resolution& resolved, const input_state& physical, const action_map* actions);
		// Created empty (not initialized) when the layer has none yet.
		layer_memory& memory(std::string_view world, std::string_view layer);
		void forget_memory(std::string_view world, std::string_view layer);

		std::vector<stack_entry> m_entries;
		std::uint64_t m_added = 0;
		std::map<std::pair<std::string, std::string>, layer_memory> m_memory;
		std::set<std::string, std::less<>> m_off_worlds;
		std::set<std::string> m_warned;
	};

	// Applies the commands in order to the stack of a set whose worlds are `worlds` (what gs::route_input knows of them); the
	// last command on a layer wins. A command that names a world that is not there, a layer that does not exist, or a layer of
	// another owner is logged and skipped: push, replace, remove, set_active and set_order touch only the layers of their own
	// `source`; set_world_active switches the whole world. Returns how many commands were applied.
	SNK_API std::size_t apply_commands(input_stack& stack, const std::vector<stack_command>& commands, const std::vector<world_ref>& worlds);

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
		// Of the keys and buttons that reached, the ones armed in the layer that declares the action (arm_actions): seen up,
		// or never held through a layer that took them since (INPUT_STACK.md 5). A press needs a binding that is armed.
		std::bitset<key_count> armed_keys;
		std::bitset<mouse_count> armed_buttons;
	};

	// What of the keys and the buttons reached one layer of the walk (the layer sees what it takes).
	struct layer_reach
	{
		std::string world;
		std::string layer;
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
		// The live layers, top first: what reached each.
		std::vector<layer_reach> layers;

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

	// After resolve: the memory of fronts of every layer of the stack follows what is physically held (`physical`: the raw
	// state, not what a world receives), and each action of the resolution learns which of its reached bindings are armed.
	// A key that is up is armed; one that is held and did not reach a layer (taken above, the layer inactive or not fed)
	// is not, until it is let go; one that is held and reached keeps what it was. A layer without memory starts with the
	// held keys disarmed. `actions`: the map of resolve, null when there is none.
	// (declared above, before input_stack, which it befriends)

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
