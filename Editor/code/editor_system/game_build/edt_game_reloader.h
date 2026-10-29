#pragma once
#include <snk/core/core_project_modules.h>
#include <snk/editor/edt_registration.h>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace edt
{
	class action_registry;
	class dialog_service;

	struct game_reloader_options
	{
		// The runtime modules of the open project's manifest; empty without a project.
		std::function<std::vector<std::string>()> modules;
		// The build of a module in the project's build output, if there is one.
		std::function<std::optional<std::filesystem::path>(const std::string& module)> build_output;
		// Swaps the running module for its new build (core::project_module_host::reload with the editor's hooks).
		std::function<core::reload_report(const std::string& module)> reload;
		// A build is running; null: never.
		std::function<bool()> building;
	};

	// Reloads the game module from its new build (#435): Build > Reload Game Module (action build.reload_game_module)
	// and after every successful build. A build that is not taken is shown as a message; the running module stays.
	// Main thread only, never from one of the module's callbacks.
	class game_reloader
	{
	public:
		static constexpr const char* k_action_id = "build.reload_game_module";

		game_reloader(dialog_service& dialogs, action_registry& actions, game_reloader_options options);

		// Why build.reload_game_module is disabled; empty when it is enabled.
		[[nodiscard]] std::string disabled_reason() const;
		// The module built into `module_path` (a file of the build output); refused for a file of no module.
		core::reload_report reload_built(const std::filesystem::path& module_path);
		// Empty `module`: the first runtime module that has a build.
		core::reload_report reload(const std::string& module);

	private:
		std::optional<std::string> pick(const std::string& module) const;
		core::reload_report tell(const std::string& module, core::reload_report report);

		dialog_service& m_dialogs;
		game_reloader_options m_options;
		registration m_action;
	};
}
