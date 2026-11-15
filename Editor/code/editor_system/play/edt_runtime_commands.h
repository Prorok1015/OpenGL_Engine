#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_registration.h>
#include <functional>
#include <vector>

namespace scn
{
	class context_set;
}

namespace edt
{
	class play_session;

	struct runtime_commands_deps
	{
		// The session; null function or a null result: there is no Play.
		std::function<play_session*()> play{};
		// The set "play"; null when it does not exist. Its layer's catalog says how the components are written.
		std::function<scn::context_set*()> play_set{};
	};

	// runtime.query / runtime.get (#552): a read-only look at the live entities of a world of the set "play", which
	// scene.query / node.get (the document) cannot show: clones and spawned things have no node. Agent category `read`,
	// nothing is asked. An entity is addressed by its whole entt id (index and version: a slot that was reused is
	// `not_found`, not another entity). Components are written by scn::entity_writer, the same JSON a snapshot holds;
	// a type without a schema is only named (`unsaved`). Every answer says it is the state of Play (`source`) and at
	// which frame; a cursor of another frame is stale, so pause the game to walk through all of it. Outside Play: not_found.
	[[nodiscard]] std::vector<registration> register_runtime_commands(cmd::command_registry& commands, runtime_commands_deps deps);

	// Limits of one answer; the page and the entity are cut at them and say so.
	inline constexpr std::size_t runtime_entity_max_bytes = 256 * 1024;
	inline constexpr std::size_t runtime_page_max_bytes = 256 * 1024;
}
