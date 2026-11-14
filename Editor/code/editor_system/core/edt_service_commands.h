#pragma once
#include <snk/editor/edt_api.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/editor/edt_registration.h>
#include <snk/command/cmd_log_commands.h>
#include <snk/command/cmd_registry.h>
#include <vector>

namespace edt
{
	// Exported for editor_system and the white-box tests only: this header is not installed, so it is not API.
	//
	// Commands over the editor's services, for scripts and agents. Each returns the registrations it made (keep them
	// as long as the commands should exist); a refused one is left out.
	//
	// job.list {} -> {jobs: [job]}
	// job.get {id} -> job (a running job or one of the last finished); not_found otherwise
	// job.cancel {id} -> {id, cancelled: true}; not_found for an unknown id, failed for one that already finished
	// job = {id, label, status: running|done|failed|cancelled, fraction, note, cancel_requested, error?, result?, result_dropped?}
	// (result: what a done job started with job_service::start_with_result returned; result_dropped: true when it was
	// dropped to keep the stored results under job_service::RESULT_BUDGET, oldest first)
	//
	// `filter` (the overloads; may be null, must outlive the commands) is what log.read asks (#935): a caller it does not
	// show everything gets `label`, `note` and `error` of a job, `text` of a dialog and `file`, `message` and `notes` of a
	// problem through its `redact`, and no problem of the sources `build` and `reload` while it hides "editor/build".
	// A job's `result` is the answer of the command that started it: only its message fields
	// (k_agent_job_result_fields, agent_job_result) go through `redact`, the rest goes out as that command made it.
	[[nodiscard]] SNK_EDITOR_API std::vector<registration> register_job_commands(cmd::command_registry& commands, job_service& jobs);
	[[nodiscard]] SNK_EDITOR_API std::vector<registration> register_job_commands(cmd::command_registry& commands, job_service& jobs,
		const cmd::log_read_filter* filter);

	// A job that threw is logged and shown to the person as an error message dialog (the dialog is skipped while the
	// dialog service has no registrar). Cancelled and finished jobs are not reported. `jobs` and `dialogs` must
	// outlive the returned subscription.
	[[nodiscard]] SNK_EDITOR_API connection report_failed_jobs(editor_events& events, const job_service& jobs, dialog_service& dialogs);

	// dialog.list {} -> {dialogs: [{id, kind, text, answer_schema, on_screen}]}
	// dialog.answer {id, answer} -> {id, resolved}; see dialog_service::answer for the answers and the error codes
	[[nodiscard]] SNK_EDITOR_API std::vector<registration> register_dialog_commands(cmd::command_registry& commands, dialog_service& dialogs);
	[[nodiscard]] SNK_EDITOR_API std::vector<registration> register_dialog_commands(cmd::command_registry& commands, dialog_service& dialogs,
		const cmd::log_read_filter* filter);

	// problems.list {source?, severity?: error|warning|note} -> {problems: [problem]}; both arguments filter
	// problem = {source, severity, file, line, column, code, message, notes: [string]}; file "" and line/column 0 mean none
	[[nodiscard]] SNK_EDITOR_API std::vector<registration> register_problem_commands(cmd::command_registry& commands, const problem_list& problems);
	[[nodiscard]] SNK_EDITOR_API std::vector<registration> register_problem_commands(cmd::command_registry& commands, const problem_list& problems,
		const cmd::log_read_filter* filter);
}
