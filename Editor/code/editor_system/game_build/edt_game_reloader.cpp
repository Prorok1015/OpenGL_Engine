#include "edt_game_reloader.h"
#include "edt_game_builder.h"
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
}

std::string edt::game_reloader::disabled_reason() const
{
	if (m_options.building && m_options.building()) {
		return "A build is running";
	}
	if (!m_options.modules || m_options.modules().empty()) {
		return "The project names no game module";
	}
	if (!pick({})) {
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

core::reload_report edt::game_reloader::reload_built(const std::filesystem::path& module_path)
{
	if (m_options.modules) {
		for (const auto& name : m_options.modules()) {
			if (module_path.filename() == std::filesystem::path{ module_file_name(name) }) {
				return reload(name);
			}
		}
	}
	return tell({}, { core::reload_status::refused, "the build is no module of the project" });
}

core::reload_report edt::game_reloader::reload(const std::string& module)
{
	if (m_options.building && m_options.building()) {
		return tell(module, { core::reload_status::refused, "a build is running" });
	}
	const std::optional<std::string> name = pick(module);
	if (!name) {
		return tell(module, { core::reload_status::refused, "it has not been built" });
	}
	return tell(*name, m_options.reload(*name));
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
