#pragma once
#include <snk/input/inp_actions.h>
#include <snk/input/inp_events.hpp>
#include <snk/input/inp_input_stack.h>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/common/snk_api.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ecs { class system_factory; }

// The frame's state of the game actions in a world and their edges on the world's bus (WORLD_BUS.md 4.2).
namespace inp
{
	enum class action_edge : std::uint8_t { pressed, released };

	// `action` is an id of the map the world reads. The released sent when the map changes carries the id of the old map:
	// keep the ids taken by name at start, do not turn this id into a name through the new map in that frame.
	struct action_event
	{
		action_id action = invalid_action;
		action_edge edge = action_edge::pressed;
	};

	// Written into each world's ctx by gs::route_input: the actions the world reads. Empty for a world that gets none: a
	// world reads the actions its layers declare (INPUT_STACK.md 3.2); a layer that declares none and is the base layer
	// of the first world in the level file that runs inp::update_actions declares all of them (M6, the default).
	struct action_map_ref
	{
		std::shared_ptr<const action_map> map{};
	};

	// Written into each world's ctx by gs::route_input next to the map (#624): by action id, what of the action reached a
	// layer of the world that declares it and which of that is armed in the memory of fronts of the layer. An action is
	// down when a binding of it is down and reached, and it is pressed only through a binding that is armed too; an action
	// no layer declares is never down. Empty (not written): the state alone decides, as before layers declared actions.
	struct action_input
	{
		std::vector<action_reach> reach{};
	};

	struct injected_edge
	{
		action_id action = invalid_action;
		action_edge edge = action_edge::pressed;
	};

	// Written into each world's ctx by gs::route_input (#624): the edges queued with input_router::inject since the last
	// frame, with the action names turned into ids of the map the world reads. Applied by inp::update_actions in the same
	// frame, in order, as real presses and releases (INPUT_STACK.md 7); the layers play no part. A Pause and a world frozen
	// in a Step do not run inp::update_actions: the edges wait in the router until a frame that does.
	struct action_injection
	{
		std::vector<injected_edge> edges{};
	};

	// Written by gs::route_input. `bound`: the router fed this world the input this frame. `frozen`: not bound while its
	// set runs a Step, so the actions stay as the pause left them instead of being let go.
	struct input_bound
	{
		bool bound = false;
		bool frozen = false;
	};

	class fixed_action_state;

	// An action goes down (pressed) only through a binding that was seen up while the input was bound ("armed", INPUT_STACK.md
	// M4/5): coming back to the game view with a key held gives no press. Losing the binding lets every held action go (one
	// released). Without action_input (no layers declared the actions) the action itself is armed once all its keys are seen
	// up. With it the memory of fronts of the layers says which binding is armed, so a key held and taken for good does not
	// stop the press of another binding of the action. A new map (live reload of actions.desc, the reader moving to another
	// world) first releases what the old one held; the bindings that are down at that moment give no press until they are
	// seen up.
	class SNK_API action_state
	{
	public:
		bool held(action_id id) const;
		bool pressed(action_id id) const;
		bool released(action_id id) const;
		// Through the map of the world: slower than by id; false for a name the map does not have (warned once). Main
		// thread only (the map remembers the names it warned about): take the ids by name once at init and keep them.
		bool held(std::string_view name) const;
		bool pressed(std::string_view name) const;
		bool released(std::string_view name) const;

		// One frame of inp::update_actions; the edges also go to `events` when it is given.
		// `injected`: edges that do not come from the keyboard. A pressed one holds the action until a released one comes,
		// whatever the real keys do, and survives losing the binding; a released one lets go unless a binding that gives the
		// action a press (reached, armed, not suppressed) still holds it.
		void update(const action_map_ref& actions, const input_state& state, const input_bound& bound,
			ecs::event_writer<action_event>* events, fixed_action_state* fixed = nullptr, const action_input* reached = nullptr,
			const action_injection* injected = nullptr);

	private:
		struct slot
		{
			bool held = false;
			bool pressed = false;
			bool released = false;
			bool armed = false;
			// Held by an injected press, not yet let go by an injected release.
			bool injected = false;
			// With action_input: the bindings (by index) that were down when the map came in; they give no press until seen up.
			// An action has few bindings; those from the 65th on are never suppressed.
			std::uint64_t suppressed = 0;
		};

		const slot* at(action_id id) const;
		action_id find(std::string_view name) const;

		std::shared_ptr<const action_map> m_map{};
		std::vector<slot> m_slots{};
	};

	// The actions as fixed_update systems read them (WORLD_BUS.md 4.4). A frame may run 0 fixed steps or several: the
	// frame's edges wait in a short queue per action and each step takes at most one, in order, so a press is seen by
	// exactly one step and a press let go before any step still gives one step with pressed, then one with released.
	// held() is the frame's level in every step, so a step's edge may disagree with it (pressed and let go in one frame:
	// the step with pressed sees held() false): test the edge or the level, not both together. The edge of a step lasts
	// until the next begin_step, so it means something only inside fixed_update. world_context starts each step.
	class SNK_API fixed_action_state
	{
	public:
		static constexpr std::size_t queue_limit = 8;

		bool held(action_id id) const;
		bool pressed(action_id id) const;
		bool released(action_id id) const;
		// Main thread only, like action_state: take the ids by name once at init.
		bool held(std::string_view name) const;
		bool pressed(std::string_view name) const;
		bool released(std::string_view name) const;

		// Before each fixed step: every action takes the next edge of its queue, if any.
		void begin_step();

		// Fed by action_state::update. A new map drops the queues; an action the fixed steps saw pressed under the old
		// one gets one released (old id) for the next step.
		void set_map(std::shared_ptr<const action_map> map);
		const std::shared_ptr<const action_map>& map() const { return m_map; }
		// A full queue drops its two oldest edges (a press and its release): the steps still see them in pairs.
		void push(action_id id, action_edge edge);
		void set_held(action_id id, bool held);

	private:
		struct slot
		{
			std::deque<action_edge> queue{};
			bool has_step_edge = false;
			action_edge step_edge = action_edge::pressed;
			bool held = false;
			// What the steps were last shown: pressed and not yet released.
			bool shown_down = false;
			bool warned = false;
		};

		const slot* at(action_id id) const;
		action_id find(std::string_view name) const;

		std::shared_ptr<const action_map> m_map{};
		std::vector<slot> m_slots{};
	};

	// What inp::update_actions did in a world, for MCP's input.actions (WORLD_BUS.md 3.7): by action id of `map`, the
	// update that gave the action's last edge. Starts again when the world reads another map.
	struct action_trace
	{
		struct edge
		{
			// Of `updates`: 1 for the first frame that ran inp::update_actions.
			std::uint64_t update = 0;
			action_edge kind = action_edge::pressed;
		};

		std::uint64_t updates = 0;
		std::shared_ptr<const action_map> map{};
		std::vector<std::optional<edge>> last_edges{};
	};

	// The system "inp::update_actions": phase input, after inp::update_input_state, not run in Edit or Pause.
	SNK_API void update_actions_system(ecs::event_writer<action_event>& events, const input_state& state,
		const input_bound& bound, const action_map_ref& actions, const action_input& reached, const action_injection& injected,
		action_state& out, fixed_action_state& fixed);
	// The same, keeping the trace too: what the engine registers as "inp::update_actions" (module API 3.14).
	SNK_API void update_and_trace_actions_system(ecs::event_writer<action_event>& events, const input_state& state,
		const input_bound& bound, const action_map_ref& actions, const action_input& reached, const action_injection& injected,
		action_state& out, fixed_action_state& fixed, action_trace& trace);
	// action_event as the bus journal shows it: {"action": id, "edge": "pressed" | "released"}.
	SNK_API std::string action_event_json(const action_event& event);
}
