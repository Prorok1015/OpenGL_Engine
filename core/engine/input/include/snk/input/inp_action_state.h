#pragma once
#include <snk/input/inp_actions.h>
#include <snk/input/inp_events.hpp>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/common/snk_api.h>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace ecs { class system_factory; }

// The frame's state of the game actions in a world and their edges on the world's bus (WORLD_BUS.md 4.2).
namespace inp
{
	enum class action_edge : std::uint8_t { pressed, released };

	struct action_event
	{
		action_id action = invalid_action;
		action_edge edge = action_edge::pressed;
	};

	// Written into each world's ctx by gs::route_input: the actions the world reads. Empty for a world that gets none
	// (until #624 only the first world of a set in the level file that runs inp::update_actions, INPUT_STACK.md M6).
	struct action_map_ref
	{
		std::shared_ptr<const action_map> map{};
	};

	// Written by gs::route_input. `bound`: the router fed this world the input this frame. `frozen`: not bound while its
	// set runs a Step, so the actions stay as the pause left them instead of being let go.
	struct input_bound
	{
		bool bound = false;
		bool frozen = false;
	};

	// An action goes down (pressed) only once it was seen up while the input was bound ("armed", INPUT_STACK.md M4): coming
	// back to the game view with a key held gives no press. Losing the binding lets every held action go (one released).
	// A new map (live reload of actions.desc, the reader moving to another world) first releases what the old one held;
	// then every action starts up and disarmed, and arms once its keys are seen up, so a key held across the change gives
	// no press. "Up" is what the world receives: a layer that takes a key (a text field) lets the action go and re-arms it.
	class SNK_API action_state
	{
	public:
		bool held(action_id id) const;
		bool pressed(action_id id) const;
		bool released(action_id id) const;
		// Through the map of the world: slower than by id; false for a name the map does not have (warned once).
		bool held(std::string_view name) const;
		bool pressed(std::string_view name) const;
		bool released(std::string_view name) const;

		// One frame of inp::update_actions; the edges also go to `events` when it is given.
		void update(const action_map_ref& actions, const input_state& state, const input_bound& bound,
			ecs::event_writer<action_event>* events);

	private:
		struct slot
		{
			bool held = false;
			bool pressed = false;
			bool released = false;
			bool armed = false;
		};

		const slot* at(action_id id) const;
		action_id find(std::string_view name) const;

		std::shared_ptr<const action_map> m_map{};
		std::vector<slot> m_slots{};
	};

	// The system "inp::update_actions": phase input, after inp::update_input_state, not run in Edit or Pause.
	SNK_API void update_actions_system(ecs::event_writer<action_event>& events, const input_state& state,
		const input_bound& bound, const action_map_ref& actions, action_state& out);
}
