#include "edt_editor_quit.h"
#include <snk/command/cmd_types.h>
#include <snk/common/logger/engine_log.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <memory>
#include <string>
#include <utility>

namespace json = boost::json;

edt::registration edt::register_editor_quit(cmd::command_registry& commands, editor_quit_sources sources)
{
	auto source = std::make_shared<editor_quit_sources>(std::move(sources));
	cmd::command_desc desc;
	desc.id = "editor.quit";
	desc.summary = "Stop the headless editor (its stdio agent only); unsaved changes need discard_unsaved";
	desc.args_schema = json::parse(R"({"type":"object","properties":{"discard_unsaved":{"type":"boolean"}},"additionalProperties":false})");
	desc.result_schema = json::parse(R"({"type":"object","properties":{"stopping":{"type":"boolean"}},"required":["stopping"]})");
	desc.mutates = true;
	desc.category = cmd::agent_category::editor_control;
	desc.invoke = [source](const cmd::call& call) {
		const std::string session = source->stdio_session ? source->stdio_session() : std::string{};
		if (call.from.kind != cmd::origin_kind::agent || session.empty() || call.from.session != session) {
			return cmd::result::failure(cmd::codes::denied, "only the agent on the editor's stdin/stdout may stop it");
		}
		const json::value* flag = call.args.as_object().if_contains("discard_unsaved");
		const bool discard = flag != nullptr && flag->as_bool();
		const bool dirty = source->is_dirty && source->is_dirty();
		if (dirty && !discard) {
			return cmd::result::failure(cmd::codes::needs_input, "the level has unsaved changes: pass discard_unsaved to drop them");
		}
		if (dirty && source->discard_changes) {
			source->discard_changes();
		}
		egLOG_INFO("agent", "editor.quit: the editor stops at the end of this frame");
		if (source->request_stop) {
			source->request_stop();
		}
		return cmd::result::success(json::object{ { "stopping", true } });
	};
	return commands.add(std::move(desc));
}

void edt::on_stdio_input_closed(bool headless, const editor_quit_sources& sources)
{
	if (!headless) {
		egLOG_INFO("agent", "MCP stdio client disconnected");
		return;
	}
	if (sources.is_dirty && sources.is_dirty()) {
		egLOG_WARN("agent", "stdin closed with unsaved changes: exiting without saving");
		if (sources.discard_changes) {
			sources.discard_changes();
		}
	} else {
		egLOG_INFO("agent", "stdin closed: the headless editor stops");
	}
	if (sources.request_stop) {
		sources.request_stop();
	}
}
