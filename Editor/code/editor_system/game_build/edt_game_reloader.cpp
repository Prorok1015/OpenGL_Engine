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
		return json::parse(R"({"type":"object","properties":{"status":{"type":"string","enum":["reloaded","refused","rolled_back","lost"]},)"
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
			m_options.next_frame("edt/reload_game_module/" + built.module, [this, alive = std::weak_ptr<int>(m_alive), module = built.module] {
				if (!alive.expired()) {
					reload(module);
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
	core::reload_report report = attempt(module, who);
	return tell(who, std::move(report));
}

core::reload_report edt::game_reloader::reload_quiet(const std::string& module)
{
	std::string who = module;
	return attempt(module, who);
}

core::reload_report edt::game_reloader::attempt(const std::string& module, std::string& who)
{
	if (m_options.building && m_options.building()) {
		return { core::reload_status::refused, "a build is running" };
	}
	const std::optional<std::string> name = pick(module);
	if (!name) {
		return { core::reload_status::refused, "it has not been built" };
	}
	who = *name;
	return m_options.reload(*name);
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
