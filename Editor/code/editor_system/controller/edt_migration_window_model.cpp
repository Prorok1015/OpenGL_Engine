#include "edt_migration_window_model.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/common/logger/engine_log.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <cstddef>
#include <format>
#include <utility>

namespace {
	namespace json = boost::json;

	// A report.md of a big project stays readable; the rest is in .snk/migrations/<id>/report.md.
	constexpr std::size_t k_text_cap = 256 * 1024;
	constexpr std::size_t k_line_cap = 1024;

	// Text from files and the plan: control characters (but tab and line feed) become spaces, the length is capped.
	std::string clean(std::string_view text, std::size_t cap, bool multiline)
	{
		std::string out;
		out.reserve(text.size() < cap ? text.size() : cap);
		for (const char c : text) {
			if (out.size() >= cap) {
				out += multiline ? "\n..." : "...";
				break;
			}
			const auto byte = static_cast<unsigned char>(c);
			if (byte == '\n' && multiline) out.push_back('\n');
			else if (byte < 0x20 || byte == 0x7f) out.push_back(' ');
			else out.push_back(c);
		}
		return out;
	}

	std::string string_of(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		return value && value->is_string() ? clean(value->as_string(), k_line_cap, false) : std::string{};
	}

	std::string text_of(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		return value && value->is_string() ? clean(value->as_string(), k_text_cap, true) : std::string{};
	}

	std::int64_t int_of(const json::object& object, std::string_view key, std::int64_t fallback)
	{
		const json::value* value = object.if_contains(key);
		if (value && value->is_int64()) return value->as_int64();
		if (value && value->is_uint64()) return static_cast<std::int64_t>(value->as_uint64());
		return fallback;
	}

	bool bool_of(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		return value && value->is_bool() && value->as_bool();
	}

	const json::array* array_of(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		return value && value->is_array() ? &value->as_array() : nullptr;
	}

	template <class each>
	void for_each_object(const json::object& object, std::string_view key, each&& fn)
	{
		if (const json::array* items = array_of(object, key)) {
			for (const json::value& item : *items) {
				if (item.is_object()) fn(item.as_object());
			}
		}
	}

	std::vector<std::string> warnings_of(const json::object& file)
	{
		std::vector<std::string> out;
		for_each_object(file, "warnings", [&out](const json::object& warning) {
			out.push_back(std::format("{} v{}: {}", string_of(warning, "type"), int_of(warning, "from", 0), string_of(warning, "message")));
		});
		return out;
	}

	edt::migration_window_model::plan_view plan_of(const json::object& answer)
	{
		edt::migration_window_model::plan_view out;
		out.code = static_cast<int>(int_of(answer, "code", -1));
		out.text = text_of(answer, "text");
		const json::value* planned = answer.if_contains("plan");
		if (!planned || !planned->is_object()) return out;
		const json::object& plan = planned->as_object();
		for_each_object(plan, "files", [&out](const json::object& file) {
			edt::migration_window_model::file_entry entry;
			entry.path = string_of(file, "path");
			for_each_object(file, "steps", [&entry](const json::object& step) {
				entry.steps.push_back(std::format("{} v{} -> v{}: {}", string_of(step, "type"), int_of(step, "from", 0), int_of(step, "to", 0),
					string_of(step, "summary")));
			});
			entry.warnings = warnings_of(file);
			out.files.push_back(std::move(entry));
		});
		for_each_object(plan, "project_steps", [&out](const json::object& step) {
			out.project_steps.push_back(std::format("{} ({}): {}", string_of(step, "id"), string_of(step, "phase"), string_of(step, "summary")));
		});
		for_each_object(plan, "skipped", [&out](const json::object& file) {
			out.skipped.push_back(std::format("{} ({}): {}", string_of(file, "path"), string_of(file, "kind"), string_of(file, "reason")));
		});
		out.refused = string_of(plan, "refused");
		if (const json::array* unfinished = array_of(plan, "unfinished")) {
			for (const json::value& id : *unfinished) {
				if (id.is_string()) out.unfinished.push_back(clean(id.as_string(), k_line_cap, false));
			}
		}
		if (const json::value* manifest = plan.if_contains("manifest"); manifest && manifest->is_object() && bool_of(manifest->as_object(), "outdated")) {
			const json::object& state = manifest->as_object();
			out.manifest = std::format("Engine {} -> {}", string_of(state, "engine_version"),
				bool_of(state, "completes") ? "this engine" : "kept (files of an unloaded module stay)");
		}
		return out;
	}

	edt::migration_window_model::report_view report_of(const json::object& answer)
	{
		edt::migration_window_model::report_view out;
		if (const json::value* failure = answer.if_contains("error"); failure && failure->is_object()) {
			const std::string code = string_of(failure->as_object(), "code");
			out.error_code = code.empty() ? "failed" : code;
			out.error_text = string_of(failure->as_object(), "message");
		} else if (failure && failure->is_string()) {
			// code 6 with the reason the apply stopped
			out.error_code = "failed";
			out.error_text = clean(failure->as_string(), k_line_cap, false);
		}
		out.code = static_cast<int>(int_of(answer, "code", -1));
		out.status = string_of(answer, "status");
		out.id = string_of(answer, "id");
		out.refused = string_of(answer, "refused");
		out.text = text_of(answer, "text");
		if (const json::value* report = answer.if_contains("report"); report && report->is_object()) {
			for_each_object(report->as_object(), "files", [&out](const json::object& file) {
				const std::string path = string_of(file, "path");
				for (std::string& warning : warnings_of(file)) out.warnings.push_back(std::format("{}: {}", path, warning));
			});
		}
		if (const json::value* level = answer.if_contains("level"); level && level->is_object()) {
			const json::object& state = level->as_object();
			const std::string tag = string_of(state, "tag");
			const std::string name = tag.empty() ? std::string("The level") : "Level " + tag;
			out.level = bool_of(state, "reopened") ? name + " reopened"
				: std::format("{} not reopened: {}", name, string_of(state, "reason"));
		}
		return out;
	}
}

edt::migration_window_model::migration_window_model(editor_events& events, ports ports)
	: m_events(events), m_ports(std::move(ports))
{
	m_finished = m_events.job_finished.subscribe(
		[this](std::uint64_t id, job_status status, const std::string& error, const std::optional<json::value>& result) {
			finished(id, status, error, result);
		});
}

std::string edt::migration_window_model::hint_of(std::string_view code)
{
	if (code == "no_project") return "Open a project first.";
	if (code == "busy") return "Something else is running. Try again once it has ended.";
	if (code == "needs_input") return "Save or discard the unsaved changes and finish the edit in progress, then press Migrate again.";
	if (code == "denied") return "The project changed since the plan was made. Refresh the plan and try again.";
	if (code == "failed") return "See the log and .snk/migrations in the project for what happened.";
	return {};
}

std::string edt::migration_window_model::outcome_text(int code)
{
	switch (code) {
	case k_code_done: return "Migrated";
	case k_code_refused: return "Refused: nothing was written";
	case k_code_partial: return "Migrated; some files were left as they are";
	case k_code_rolled_back: return "Failed and rolled back: the project is as it was";
	case k_code_rollback_failed: return "Failed and the rollback did not finish: the project is partly migrated";
	default: return "Not migrated";
	}
}

void edt::migration_window_model::clear_error()
{
	m_error_code.clear();
	m_error_text.clear();
	m_error_hint.clear();
}

void edt::migration_window_model::fail(std::string code, std::string_view text)
{
	m_error_hint = hint_of(code);
	m_error_code = std::move(code);
	m_error_text = clean(text, k_line_cap, false);
	egLOG_WARN("project/migrate", "migration window: {}: {}", m_error_code, m_error_text);
}

void edt::migration_window_model::request_plan()
{
	if (m_phase == phase::running) return;
	m_phase = phase::planning;
}

void edt::migration_window_model::tick()
{
	if (m_phase != phase::planning) return;
	clear_error();
	m_plan.reset();
	m_report.reset();
	if (!m_ports.invoke) {
		m_phase = phase::idle;
		fail("failed", "project migration is not available in this editor");
		return;
	}
	const cmd::result answer = m_ports.invoke("project.migration_plan", json::object{});
	if (!answer.ok || !answer.value.is_object()) {
		m_phase = phase::idle;
		fail(answer.ok || answer.code.empty() ? std::string("failed") : answer.code, answer.ok ? std::string_view("the plan has no answer") : answer.error);
		return;
	}
	m_plan = plan_of(answer.value.as_object());
	m_phase = phase::plan_shown;
}

void edt::migration_window_model::migrate()
{
	if (!can_migrate()) return;
	clear_error();
	const cmd::result started = m_ports.invoke("project.migrate", json::object{});
	if (!started.ok) {
		fail(started.code.empty() ? std::string("failed") : started.code, started.error);
		return;
	}
	const json::value* job = started.value.is_object() ? started.value.as_object().if_contains("job") : nullptr;
	if (!job || !job->is_number()) {
		fail("failed", "no migration job was started");
		return;
	}
	m_job = job->to_number<std::uint64_t>();
	m_report.reset();
	m_phase = phase::running;
}

void edt::migration_window_model::close()
{
	if (m_phase == phase::running) return;
	m_phase = phase::idle;
	m_plan.reset();
	m_report.reset();
	clear_error();
}

void edt::migration_window_model::open_file(const std::string& path) const
{
	if (m_ports.open_file && !path.empty()) m_ports.open_file(path);
}

void edt::migration_window_model::finished(std::uint64_t id, job_status status, const std::string& error,
	const std::optional<json::value>& result)
{
	if (!m_job || *m_job != id) return;
	m_job.reset();
	m_phase = phase::report_shown;
	report_view report;
	if (status == job_status::cancelled) {
		report.error_code = "cancelled";
		report.error_text = "the migration job was cancelled before it wrote anything";
	} else if (status == job_status::failed) {
		report.error_code = "failed";
		report.error_text = clean(error, k_line_cap, false);
	} else if (!result || !result->is_object()) {
		report.error_code = "failed";
		report.error_text = "the migration job gave no answer";
	} else {
		report = report_of(result->as_object());
	}
	if (!report.error_code.empty()) {
		egLOG_WARN("project/migrate", "migration window: {}: {}", report.error_code, report.error_text);
	}
	m_report = std::move(report);
}

edt::registration edt::register_migration_window_action(action_registry& actions, std::function<bool()> has_project,
	std::function<void()> open)
{
	return actions.add({ .id = std::string(k_migration_window_action), .label = "Migrate Project...", .menu_path = "Project",
		.disabled_tooltip = "Open a project first",
		.execute = no_args(std::move(open)),
		.expose_as_tool = false,
		.enabled = std::move(has_project),
		.category = cmd::agent_category::not_for_agents });
}
