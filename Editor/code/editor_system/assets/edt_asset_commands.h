#pragma once
#include "edt_asset_ops.h"
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_registration.h>
#include <boost/json/value.hpp>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cmd { class command_registry; }

// asset.rename, asset.move, asset.delete (EPIC-57 section 6.4, #775): the MCP face of asset_ops. Each is category scene_edit
// with risk confirm, so an agent's call is a question to a person; a policy file may lift it for asset.rename and asset.move by
// name (as for desc.write), never for asset.delete: that one is done only on the ticket a person approved for this very call
// (`force` too, and the question lists what would break). The plan of a question is recorded with a fingerprint
// (describe_before_allowed_invoke), and an agent's run builds the plan again: no record (a question that said "will be refused"
// leaves none) is `denied`, another plan is `stale` (`plan_changed`), and nothing is done. A person's call (origin ui, or a test)
// is not asked: the asset browser (PR 3) runs the same commands.
namespace edt
{
	class editor_events;

	struct asset_command_sources
	{
		// The operations; null (or an empty function): no project is open.
		std::function<const asset_ops*()> operations;
		// Why nothing may be written into the project now (a migration runs or stopped half way, #761): `busy`.
		std::function<std::optional<std::string>()> write_blocker;
		// Why the asset index cannot judge a guid now (#968): `busy`, the move is paired by guid and nothing may be moved before it is built.
		std::function<std::optional<std::string>()> asset_index_wait;
		// Called with the report of each operation that was done (ok), before the answer: the project history takes its journal.
		std::function<void(const operation_report&)> done;
		// agent_action after each change an agent made; null: none.
		editor_events* events = nullptr;
	};

	[[nodiscard]] std::vector<registration> register_asset_commands(cmd::command_registry& commands, asset_command_sources sources);

	// The command result code of a refusal: busy where waiting or a person's save helps, needs_input where `force` is the decision.
	const char* command_code_of(refusal_code code);
	// {code, reason, referrers: [{file, json_pointer, kind, via}], skipped: [file]}: what is in the payload of a refused call.
	boost::json::value refusal_json(const refusal& why);
	// {operation, action, moved, rewritten, could_not_rewrite, warnings, broken_references}.
	boost::json::value operation_report_json(const operation_report& report);
	// What a person is asked before the call runs: the files moved, the references rewritten or left broken (`force`), capped;
	// "This request will be refused: ..." when the plan is refused.
	std::string operation_question(const operation_plan& plan, bool force);
}
