#include "edt_game_reloader.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/common/logger/engine_log.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <algorithm>
#include <format>
#include <utility>

namespace json = boost::json;

namespace {

	json::value args_schema()
	{
		return json::parse(R"({"type":"object","properties":{"module":{"type":"string"}},"additionalProperties":false})");
	}

	json::value result_schema()
	{
		return json::parse(R"({"type":"object","properties":{"status":{"type":"string","enum":["reloaded","refused","rolled_back","lost","queued"]},)"
			R"("message":{"type":"string"}},"required":["status","message"],"additionalProperties":false})");
	}

}

std::vector<std::string> edt::project_module_names(const prj::project_context& project)
{
	std::vector<std::string> names;
	for (const auto kind : { prj::module_kind::runtime, prj::module_kind::editor }) {
		for (const auto& module : project.data.modules) {
			if (module.kind == kind) names.push_back(module.name);
		}
	}
	return names;
}

edt::game_reloader::game_reloader(dialog_service& dialogs, action_registry& actions, game_reloader_options options)
	: m_dialogs(dialogs)
	, m_options(std::move(options))
{
	cmd::command_desc command;
	command.id = k_action_id;
	command.summary = "Reload Game Module";
	command.category = cmd::agent_category::build;
	command.args_schema = args_schema();
	command.result_schema = result_schema();
	command.expose_as_tool = true;
	command.mutates = true;
	// It runs the project's code: like build.game_module, never for an agent without the person's approval.
	command.risk = cmd::danger::deny_by_default;
	command.enabled = [this] { return disabled_reason().empty(); };
	command.invoke = [this](const cmd::call& request) {
		if (const std::string reason = disabled_reason(); !reason.empty()) return action_result::failure("disabled", reason);
		std::string module;
		if (const json::value* chosen = request.args.as_object().if_contains("module")) module = std::string(chosen->as_string());
		const bool unloads_editors = m_options.unloads_editor_modules && std::ranges::any_of(targets(module), [this](const std::string& name) {
			return m_options.unloads_editor_modules(name);
		});
		if (m_options.each_frame && ((m_options.background_building && m_options.background_building()) || !defer_reason().empty() || waits(module) || unloads_editors)) {
			return queue_action_reload(module);
		}
		const core::reload_report report = reload(module);
		return action_result::success(json::object{ { "status", core::to_string(report.status) }, { "message", report.message } });
	};

	action_ui ui;
	ui.label = "Reload Game Module";
	ui.menu_path = "Build";
	ui.disabled_tooltip = "Open a project and build its game module first";
	ui.activate = [this, &actions] {
		const auto outcome = actions.invoke(k_action_id, json::object{});
		if (!outcome.ok) m_dialogs.message(message_level::warning, std::format("The game module was not reloaded: {}", outcome.error));
	};
	m_action = actions.add(std::move(command), std::move(ui));

	if (m_options.events) {
		m_built = m_options.events->game_module_built.subscribe([this](const game_module_build_result& built) {
			if (!built.reload || !m_options.next_frame) {
				return;
			}
			// One per module: two modules built at once are both reloaded.
			m_options.next_frame(std::string(k_reload_frame_id) + built.module, [this, alive = std::weak_ptr<int>(m_alive), module = built.module] {
				if (!alive.expired()) {
					reload_when_ready(module, true);
				}
			});
		});
	}
}

std::string edt::game_reloader::disabled_reason() const
{
	if (m_options.building && m_options.building()) {
		return "A build is running";
	}
	if (!m_options.modules || m_options.modules().empty()) {
		return "The project names no game module";
	}
	const auto now = std::chrono::steady_clock::now();
	if (m_build_checked == std::chrono::steady_clock::time_point{} || now - m_build_checked >= std::chrono::seconds(1)) {
		m_build_checked = now;
		m_build_found = !targets({}).empty();
	}
	if (!m_build_found) {
		return "The game module has not been built";
	}
	return {};
}

bool edt::game_reloader::is_editor(const std::string& module) const
{
	return m_options.is_editor_module && m_options.is_editor_module(module);
}

std::vector<std::string> edt::game_reloader::targets(const std::string& module) const
{
	std::vector<std::string> out;
	if (!m_options.modules || !m_options.build_output) {
		return out;
	}
	for (const auto& name : m_options.modules()) {
		if (!m_options.build_output(name)) {
			continue;
		}
		if (!module.empty()) {
			if (name == module) out.push_back(name);
			continue;
		}
		// The runtime modules come first: the first that is built is the game module, and the editor modules go with it.
		if (!is_editor(name)) {
			return { name };
		}
		out.push_back(name);
	}
	return out;
}

core::reload_report edt::game_reloader::reload(const std::string& module)
{
	return run(module, true, false);
}

edt::action_result edt::game_reloader::queue_action_reload(const std::string& module)
{
	const std::vector<std::string> names = targets(module);
	if (names.empty()) {
		const core::reload_report report = tell(module, { core::reload_status::refused, "it has not been built" });
		return action_result::success(json::object{ { "status", core::to_string(report.status) }, { "message", report.message } });
	}
	// A second click while it waits joins the wait: one reload, not two.
	const std::string reason = defer_reason();
	const bool stops_live_build = m_options.background_building && m_options.background_building();
	if (stops_live_build && m_options.stop_background) m_options.stop_background();
	for (const std::string& name : names) wait_then_reload(name, false, {}, false);
	std::string message;
	if (stops_live_build) {
		message = "the live build is stopped; the module is reloaded once it has ended";
		if (!reason.empty()) message += std::format(" and once the reload may run (it waits: {})", reason);
	} else if (!reason.empty()) {
		message = std::format("the reload waits: {}", reason);
	} else {
		message = "the reload runs on the next frame or joins the one already queued";
	}
	return action_result::success(json::object{ { "status", "queued" }, { "message", message } });
}

void edt::game_reloader::reload_when_ready(const std::string& module, bool only_if_stale)
{
	if (!m_options.each_frame || (defer_reason().empty() && !m_waiting.contains(module))) {
		run(module, true, only_if_stale);
		return;
	}
	wait_then_reload(module, false, {}, only_if_stale);
}

void edt::game_reloader::reload_quiet_when_ready(const std::string& module, std::function<void(const core::reload_report&)> done)
{
	if (!m_options.each_frame || (defer_reason().empty() && !m_waiting.contains(module))) {
		const core::reload_report report = reload_quiet(module);
		if (done) done(report);
		return;
	}
	wait_then_reload(module, true, std::move(done), true);
}

void edt::game_reloader::wait_then_reload(const std::string& module, bool quiet, std::function<void(const core::reload_report&)> done, bool only_if_stale)
{
	const auto [entry, first] = m_waiting.try_emplace(module);
	entry->second.tell = entry->second.tell || !quiet;
	entry->second.only_if_stale = entry->second.only_if_stale && only_if_stale;
	if (done) entry->second.done.push_back(std::move(done));
	if (!first) return;
	m_options.each_frame(std::string(k_wait_frame_id) + module, [this, alive = std::weak_ptr<int>(m_alive), module, logged = std::string{}]() mutable {
		if (alive.expired()) return false;
		// A stopped job ends at a later pump; its compiler or linker may write the file until then.
		if (any_build_running()) return true;
		if (const std::string reason = defer_reason(); !reason.empty()) {
			if (reason != logged) {
				egLOG_INFO("editor", "The reload of the game module '{}' waits: {}", module, reason);
				logged = reason;
			}
			return true;
		}
		auto waited = m_waiting.extract(module);
		if (waited.empty()) return false;
		const core::reload_report report = run(module, waited.mapped().tell, waited.mapped().only_if_stale);
		for (const auto& notify : waited.mapped().done) notify(report);
		return false;
	});
}

bool edt::game_reloader::waits(const std::string& module) const
{
	const std::vector<std::string> names = targets(module);
	return std::ranges::any_of(names, [this](const std::string& name) { return m_waiting.contains(name); });
}

std::string edt::game_reloader::defer_reason() const
{
	return m_options.defer ? m_options.defer() : std::string{};
}

bool edt::game_reloader::any_build_running() const
{
	return (m_options.building && m_options.building()) || (m_options.background_building && m_options.background_building());
}

core::reload_report edt::game_reloader::reload_quiet(const std::string& module)
{
	return run(module, false, true);
}

std::optional<core::reload_report> edt::game_reloader::holding_back() const
{
	if (m_options.building && m_options.building()) {
		return core::reload_report{ core::reload_status::refused, "a build is running" };
	}
	if (m_options.background_building && m_options.background_building()) {
		if (m_options.stop_background) m_options.stop_background();
		// A cancelled job ends at the next pump, and its compiler or linker may still write the file until then.
		if (m_options.background_building()) {
			return core::reload_report{ core::reload_status::refused, "the live build is being stopped; reload again in a moment" };
		}
	}
	if (const std::string reason = defer_reason(); !reason.empty()) {
		return core::reload_report{ core::reload_status::refused, std::format("it must wait: {}", reason) };
	}
	return std::nullopt;
}

core::reload_report edt::game_reloader::reload_one(const std::string& module, bool by_person, bool skip_current)
{
	const bool editor_module = is_editor(module);
	if (editor_module && skip_current && m_options.is_current && m_options.is_current(module)) {
		// The reload of the game module (or an earlier one) put this build in place: nothing to swap.
		return { core::reload_status::reloaded, "already up to date" };
	}
	if (!editor_module && m_options.before_reload) {
		if (const std::string restart = m_options.before_reload(module); !restart.empty()) {
			egLOG_WARN("editor", "The Play state is not kept across the reload of '{}': {}. Play restarts.", module, restart);
		}
	}
	return by_person && m_options.reload_by_person ? m_options.reload_by_person(module) : m_options.reload(module);
}

core::reload_report edt::game_reloader::run(const std::string& module, bool tell_it, bool skip_current)
{
	const auto show = [&](const std::string& who, core::reload_report report) {
		return tell_it ? tell(who, std::move(report)) : report;
	};
	if (auto refusal = holding_back()) {
		return show(module, std::move(*refusal));
	}
	const std::vector<std::string> names = targets(module);
	if (names.empty()) {
		return show(module, { core::reload_status::refused, "it has not been built" });
	}
	std::optional<core::reload_report> problem;
	core::reload_report last;
	for (const std::string& name : names) {
		last = show(name, reload_one(name, tell_it, skip_current));
		if (last.status != core::reload_status::reloaded && !problem) problem = last;
	}
	return problem ? *problem : last;
}

core::reload_report edt::game_reloader::tell(const std::string& module, core::reload_report report)
{
	const char* const kind = !module.empty() && m_options.is_editor_module && m_options.is_editor_module(module) ? "editor module" : "game module";
	const std::string who = module.empty() ? std::string{ "The game module" } : std::format("The {} '{}'", kind, module);
	switch (report.status) {
	case core::reload_status::reloaded:
		break;
	case core::reload_status::refused:
		m_dialogs.message(message_level::warning, std::format("{} was not reloaded: {}. The running version is kept.", who, report.message));
		break;
	case core::reload_status::rolled_back:
		m_dialogs.message(message_level::warning, std::format("{} was rolled back to the previous build: {}", who, report.message));
		break;
	case core::reload_status::lost:
		m_dialogs.message(message_level::error, std::format("{} is not loaded: {}", who, report.message));
		break;
	}
	return report;
}
