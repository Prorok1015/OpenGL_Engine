#include <snk/editor/edt_service_commands.h>
#include <snk/command/cmd_json_schema.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <optional>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace json = boost::json;

namespace {
	// One job as job.get answers it and job.list lists it.
	constexpr const char* JOB_SCHEMA = R"({
		"type": "object",
		"properties": {
			"id": { "type": "integer" },
			"label": { "type": "string" },
			"status": { "type": "string", "enum": ["running", "done", "failed", "cancelled"] },
			"fraction": { "type": "number" },
			"note": { "type": "string" },
			"cancel_requested": { "type": "boolean" },
			"error": { "type": "string" }
		},
		"required": ["id", "label", "status", "fraction", "note", "cancel_requested"],
		"additionalProperties": false
	})";
	const std::string JOB_LIST_SCHEMA = std::string(R"({"type":"object","properties":{"jobs":{"type":"array","items":)") + JOB_SCHEMA
		+ R"(}},"required":["jobs"],"additionalProperties":false})";

	constexpr const char* ID_ARGS_SCHEMA = R"({
		"type": "object",
		"properties": { "id": { "type": "integer", "minimum": 0 } },
		"required": ["id"],
		"additionalProperties": false
	})";

	// The schema accepts 3, 3.0 and a uint64 for "integer"; cmd::json_schema::integer_of reads all of them.
	std::optional<std::uint64_t> id_of(const cmd::call& request)
	{
		const auto id = cmd::json_schema::integer_of(request.args.as_object().at("id"));
		if (!id || *id < 0) return std::nullopt;
		return static_cast<std::uint64_t>(*id);
	}

	cmd::result bad_id()
	{
		return cmd::result::failure(cmd::codes::invalid_args, "id is not an integer that fits");
	}

	json::value job_json(const edt::job_service::job_info& job)
	{
		json::object out{
			{ "id", static_cast<std::int64_t>(job.id) },
			{ "label", json::string(job.label) },
			{ "status", json::string(edt::job_status_name(job.status)) },
			{ "fraction", static_cast<double>(job.fraction) },
			{ "note", json::string(job.note) },
			{ "cancel_requested", job.cancel_requested } };
		if (!job.error.empty()) out["error"] = json::string(job.error);
		return out;
	}

	void keep(std::vector<edt::registration>& out, edt::registration added)
	{
		if (added.active()) out.push_back(std::move(added));
	}
}

std::vector<edt::registration> edt::register_job_commands(cmd::command_registry& commands, job_service& jobs)
{
	std::vector<registration> out;

	cmd::command_desc list;
	list.id = "job.list";
	list.summary = "List the running background jobs (import, export, ...)";
	list.result_schema = json::parse(JOB_LIST_SCHEMA);
	list.invoke = [&jobs](const cmd::call&) {
		json::array entries;
		for (const auto& job : jobs.list()) entries.push_back(job_json(job));
		return cmd::result::success(json::object{ { "jobs", std::move(entries) } });
	};
	keep(out, commands.add(std::move(list)));

	cmd::command_desc get;
	get.id = "job.get";
	get.summary = "State of one job: running with its progress, or how a recently finished one ended";
	get.args_schema = json::parse(ID_ARGS_SCHEMA);
	get.result_schema = json::parse(JOB_SCHEMA);
	get.invoke = [&jobs](const cmd::call& request) {
		const auto parsed = id_of(request);
		if (!parsed) return bad_id();
		const std::uint64_t id = *parsed;
		const auto job = jobs.info(id);
		if (!job) return cmd::result::failure(cmd::codes::not_found, std::format("no job {}", id));
		return cmd::result::success(job_json(*job));
	};
	keep(out, commands.add(std::move(get)));

	cmd::command_desc cancel;
	cancel.id = "job.cancel";
	cancel.summary = "Ask a running job to stop; its result is discarded";
	cancel.args_schema = json::parse(ID_ARGS_SCHEMA);
	cancel.result_schema = json::parse(R"({"type":"object","properties":{"id":{"type":"integer"},"cancelled":{"type":"boolean"}},"required":["id","cancelled"],"additionalProperties":false})");
	// Stops running work and discards its result.
	cancel.mutates = true;
	cancel.invoke = [&jobs](const cmd::call& request) {
		const auto parsed = id_of(request);
		if (!parsed) return bad_id();
		const std::uint64_t id = *parsed;
		if (jobs.cancel(id)) {
			return cmd::result::success(json::object{ { "id", static_cast<std::int64_t>(id) }, { "cancelled", true } });
		}
		if (jobs.info(id)) return cmd::result::failure(cmd::codes::failed, std::format("job {} already finished", id));
		return cmd::result::failure(cmd::codes::not_found, std::format("no job {}", id));
	};
	keep(out, commands.add(std::move(cancel)));
	return out;
}

std::vector<edt::registration> edt::register_dialog_commands(cmd::command_registry& commands, dialog_service& dialogs)
{
	std::vector<registration> out;

	cmd::command_desc list;
	list.id = "dialog.list";
	list.summary = "List the modal dialogs waiting for an answer, with the schema of each answer";
	list.result_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"dialogs": {
				"type": "array",
				"items": {
					"type": "object",
					"properties": {
						"id": { "type": "integer" },
						"kind": { "type": "string", "enum": ["unsaved", "save_as", "pick_file", "message"] },
						"text": { "type": "string" },
						"answer_schema": { "type": "object" },
						"on_screen": { "type": "boolean" }
					},
					"required": ["id", "kind", "text", "answer_schema", "on_screen"],
					"additionalProperties": false
				}
			}
		},
		"required": ["dialogs"],
		"additionalProperties": false
	})");
	list.invoke = [&dialogs](const cmd::call&) {
		json::array entries;
		for (const auto& dialog : dialogs.list()) {
			entries.push_back(json::object{
				{ "id", static_cast<std::int64_t>(dialog.id) },
				{ "kind", json::string(dialog.kind) },
				{ "text", json::string(dialog.text) },
				{ "answer_schema", dialog.answer_schema },
				{ "on_screen", dialog.on_screen } });
		}
		return cmd::result::success(json::object{ { "dialogs", std::move(entries) } });
	};
	keep(out, commands.add(std::move(list)));

	cmd::command_desc answer;
	answer.id = "dialog.answer";
	answer.summary = "Answer a waiting dialog the way a click would; the answer must match its answer_schema";
	answer.args_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"id": { "type": "integer", "minimum": 0 },
			"answer": { "type": "object" }
		},
		"required": ["id", "answer"],
		"additionalProperties": false
	})");
	answer.result_schema = json::parse(R"({"type":"object","properties":{"id":{"type":"integer"},"resolved":{"type":"boolean"}},"required":["id","resolved"],"additionalProperties":false})");
	// A "save" or "discard" here decides the fate of unsaved work, the same as a click does: a person (or the project
	// policy) confirms it before an agent may do it (AI_INTEGRATION section 5).
	answer.mutates = true;
	answer.risk = cmd::danger::confirm;
	answer.invoke = [&dialogs](const cmd::call& request) {
		const auto id = id_of(request);
		if (!id) return bad_id();
		return dialogs.answer(*id, request.args.as_object().at("answer"));
	};
	keep(out, commands.add(std::move(answer)));
	return out;
}
