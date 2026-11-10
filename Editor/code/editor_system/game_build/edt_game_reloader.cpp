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
		if (m_options.each_frame && ((m_options.background_building && m_options.background_building()) || !defer_reason().empty() || waits(module))) {
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
					reload_when_ready(module);
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
		m_build_found = pick({}).has_value();
	}
	if (!m_build_found) {
		return "The game module has not been built";
	}
	return {};
}

std::optional<std::string> edt::game_reloader::pick(const std::string& module) const
{
	if (!m_options.modules || !m_options.build_output) {
		return std::nullopt;
	}
	for (const auto& name : m_options.modules()) {
		if ((module.empty() || module == name) && m_options.build_output(name)) {
			return name;
		}
	}
	return std::nullopt;
}

core::reload_report edt::game_reloader::reload(const std::string& module)
{
	std::string who = module;
	core::reload_report report = attempt(module, who, true);
	return tell(who, std::move(report));
}

edt::action_result edt::game_reloader::queue_action_reload(const std::string& module)
{
	const std::optional<std::string> name = pick(module);
	if (!name) {
		const core::reload_report report = tell(module, { core::reload_status::refused, "it has not been built" });
		return action_result::success(json::object{ { "status", core::to_string(report.status) }, { "message", report.message } });
	}
	// A second click while it waits joins the wait: one reload, not two.
	const std::string reason = defer_reason();
	if (m_options.background_building && m_options.background_building()) {
		if (m_options.stop_background) m_options.stop_background();
		wait_then_reload(*name, false, {});
		std::string message = "the live build is stopped; the module is reloaded once it has ended";
		if (!reason.empty()) message += std::format(" and once the reload may run (it waits: {})", reason);
		return action_result::success(json::object{ { "status", "queued" }, { "message", message } });
	}
	wait_then_reload(*name, false, {});
	const std::string message = reason.empty() ? std::string{ "the reload joins the one already queued" } : std::format("the reload waits: {}", reason);
	return action_result::success(json::object{ { "status", "queued" }, { "message", message } });
}

void edt::game_reloader::reload_when_ready(const std::string& module)
{
	if (!m_options.each_frame || (defer_reason().empty() && !m_waiting.contains(module))) {
		reload(module);
		return;
	}
	wait_then_reload(module, false, {});
}

void edt::game_reloader::reload_quiet_when_ready(const std::string& module, std::function<void(const core::reload_report&)> done)
{
	if (!m_options.each_frame || (defer_reason().empty() && !m_waiting.contains(module))) {
		const core::reload_report report = reload_quiet(module);
		if (done) done(report);
		return;
	}
	wait_then_reload(module, true, std::move(done));
}

void edt::game_reloader::wait_then_reload(const std::string& module, bool quiet, std::function<void(const core::reload_report&)> done)
{
	const auto [entry, first] = m_waiting.try_emplace(module);
	entry->second.tell = entry->second.tell || !quiet;
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
		const core::reload_report report = waited.mapped().tell ? reload(module) : reload_quiet(module);
		for (const auto& notify : waited.mapped().done) notify(report);
		return false;
	});
}

bool edt::game_reloader::waits(const std::string& module) const
{
	const std::optional<std::string> name = pick(module);
	return name && m_waiting.contains(*name);
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
	std::string who = module;
	return attempt(module, who, false);
}

core::reload_report edt::game_reloader::attempt(const std::string& module, std::string& who, bool by_person)
{
	if (m_options.building && m_options.building()) {
		return { core::reload_status::refused, "a build is running" };
	}
	if (m_options.background_building && m_options.background_building()) {
		if (m_options.stop_background) m_options.stop_background();
		// A cancelled job ends at the next pump, and its compiler or linker may still write the file until then.
		if (m_options.background_building()) {
			return { core::reload_status::refused, "the live build is being stopped; reload again in a moment" };
		}
	}
	if (const std::string reason = defer_reason(); !reason.empty()) {
		return { core::reload_status::refused, std::format("it must wait: {}", reason) };
	}
	const std::optional<std::string> name = pick(module);
	if (!name) {
		return { core::reload_status::refused, "it has not been built" };
	}
	who = *name;
	if (m_options.before_reload) {
		if (const std::string restart = m_options.before_reload(*name); !restart.empty()) {
			egLOG_WARN("editor", "The Play state is not kept across the reload of '{}': {}. Play restarts.", *name, restart);
		}
	}
	return by_person && m_options.reload_by_person ? m_options.reload_by_person(*name) : m_options.reload(*name);
}

core::reload_report edt::game_reloader::tell(const std::string& module, core::reload_report report)
{
	const std::string who = module.empty() ? std::string{ "The game module" } : std::format("The game module '{}'", module);
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
