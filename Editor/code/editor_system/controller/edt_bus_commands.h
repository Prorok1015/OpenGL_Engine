#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_registration.h>
#include <cstddef>
#include <functional>
#include <vector>

namespace inp
{
	class input_router;
}

namespace scn
{
	class context_set;
}

namespace edt
{
	class play_session;

	struct bus_commands_deps
	{
		// The session; null function or a null result: there is no Play.
		std::function<play_session*()> play{};
		// The set "play"; null when it does not exist.
		std::function<scn::context_set*()> play_set{};
		// Null function or result: input.actions shows no stack of layers.
		std::function<const inp::input_router*()> input{};
	};

	// world.events / input.actions (#605, WORLD_BUS.md 3.7): what the bus of a world of Play delivered last and the state of
	// the game actions, for debugging. Agent category `read`, nothing is changed and nothing is sent. Only the worlds of the
	// set "play" are shown (the editor's worlds and previews are not); outside Play both are not_found.
	[[nodiscard]] std::vector<registration> register_bus_commands(cmd::command_registry& commands, bus_commands_deps deps);

	// The events the journal of a world of Play keeps, and the most world.events returns.
	inline constexpr std::size_t play_journal_capacity = 256;
}
