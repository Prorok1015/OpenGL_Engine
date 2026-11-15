#pragma once
#include <snk/editor/edt_api.h>
#include <snk/command/cmd_log_commands.h>
#include <snk/command/cmd_registry.h>
#include <boost/json/value.hpp>
#include <array>
#include <string>
#include <string_view>

// What a caller that is not the person at the editor reads of a job's result and of a single text (#935). Compiled
// into snk_editor_api, whose job commands call it: a shared library cannot leave a symbol to editor_system.
namespace edt
{
	inline constexpr std::string_view k_agent_build_category = "editor/build";

	// The message fields of job results an agent sees redacted (vcs, forge, migration); `text` is not one: a diff.
	inline constexpr std::array<std::string_view, 7> k_agent_job_result_fields{
		"stderr_tail", "message", "error", "output", "refused", "reason", "notes",
	};
	// `text` is redacted only in an object that also has this key: every migration answer has it, a diff never does.
	inline constexpr std::string_view k_agent_job_result_text_marker = "code";
	// How many objects deep (the result itself is the first) those fields are looked for: report.files[].warnings[].
	inline constexpr int k_agent_job_result_depth = 4;

	// `text` as `from` may read it: through the filter's `redact`; as it is without a filter or without `redact`.
	SNK_EDITOR_API std::string agent_shown_text(const cmd::log_read_filter* filter, std::string_view text, const cmd::origin& from);

	// A job's result as `from` may read it: every string (or string of an array) under a name of
	// k_agent_job_result_fields, in objects and arrays of objects down to k_agent_job_result_depth, goes through
	// agent_shown_text, and so does a string `text` beside a k_agent_job_result_text_marker key; every other value
	// (a diff, a path, a subject) stays as the command made it.
	SNK_EDITOR_API boost::json::value agent_job_result(const boost::json::value& result, const cmd::log_read_filter* filter, const cmd::origin& from);
}
