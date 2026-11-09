#include <snk/game_host/game_host.h>
#include "game_host_module.h"
#include <snk/application/application.h>
#include <snk/application/app_module_loader.hpp>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_variables.h>
#include <snk/core/core_module.h>
#include <snk/core/core_project_modules.h>
#include <snk/engine/engine_module.h>
#include <snk/project/prj_project.h>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
	std::string join(const std::vector<std::string>& names)
	{
		std::string text;
		for (const auto& name : names) {
			text += (text.empty() ? "'" : ", '") + name + "'";
		}
		return text;
	}
}

int snk::game_host::run(int argc, char* argv[], std::map<std::string, app::modules::module_entry> compiled_in)
{
	// Before the configs are parsed, so the first line already goes there.
	bool log_to_stderr = false;
	for (int i = 1; i < argc; ++i) {
		if (std::string_view{ argv[i] } == "--headless") {
			log_to_stderr = true;
		}
	}
	engine::init_logger(log_to_stderr ? engine::console_target::stderr_ : engine::console_target::stdout_);

	if (!cfg::initialize_configs(argc, argv)) {
		return -1;
	}
	// Whatever the config files and --set say: a shipped game reloads nothing, writes nothing into its project (#593),
	// cannot ask about a project of another engine version and cannot run without a project.
	cfg::set_variable("res/watch_files", "false");
	cfg::set_variable("res/read_only_project", "true");
	cfg::set_variable("scene/hot_reload", "false");
	cfg::set_variable("app/non_interactive", "true");
	cfg::set_variable("project/required", "true");

	ds::app_data_storage app_storage;

	app::modules::module_loader module_loader;
	module_loader.add_module(std::make_unique<core::core_module>());
	module_loader.add_module(std::make_unique<engine::engine_module>());
	module_loader.add_module(std::make_unique<game_host_module>());

	// Exception perimeter: nothing may escape run(). initialize_all_services rolls back
	// the modules it already initialized if one throws, so shutdown below only runs
	// once initialization has fully succeeded.
	int result = -1;
	bool initialized = false;
	try {
		module_loader.register_all_services(app_storage);
		module_loader.initialize_all_services(app_storage);
		initialized = true;
		// After the host's own modules: initialize_all_services must not see the loader grow. The game modules are
		// compiled in (#592); nothing is opened from the project's build output.
		core::project_modules_options project_modules{ .compiled_in = std::move(compiled_in), .with_editor_modules = false,
			.compiled_only = true };
		auto& modules = app_storage.construct<core::project_module_host>(module_loader, app_storage, std::move(project_modules));
		const core::project_modules_report report = modules.load_all();

		// Missing (not built in) or refused (wrong name, failed to register): either way the game cannot run without it.
		if (const auto failed = modules.failed_required(report); !failed.empty()) {
			egLOG_ERROR("app", "the project requires game modules that did not load in this game: {}", join(failed));
			result = k_exit_missing_module;
		} else {
			app::application game;
			result = game.run(app_storage);
		}
		// Read before shutdown, which removes them.
		if (app_storage.has_value<prj::held_project>()) {
			const auto& held = app_storage.require<prj::held_project>();
			if (const int code = prj::exit_code_for(held); code != 0) {
				if (held.answer == prj::held_project::decision::no_project) {
					egLOG_ERROR("app", "no project: '{}' was not opened (engine {}) and this run needs a project",
						held.name, prj::to_string(held.engine));
				} else {
					egLOG_ERROR("app", "project '{}' was not opened: its engine version {} does not match this game",
						held.name, prj::to_string(held.engine));
				}
				result = code;
			}
		}
		if (const int code = app_storage.require<run_outcome>().exit_code; result == 0 && code != 0) {
			result = code;
		}
	} catch (const prj::engine_version_error& e) {
		egLOG_ERROR("app", "{}", e.what());
		result = prj::k_exit_engine_version;
	} catch (const prj::project_error& e) {
		egLOG_ERROR("app", "{}", e.what());
		result = prj::k_exit_project_error;
	} catch (const std::exception& e) {
		egLOG_ERROR("app", "fatal error: {}", e.what());
		result = -1;
	} catch (...) {
		egLOG_ERROR("app", "fatal error: unknown exception");
		result = -1;
	}

	if (initialized) {
		try {
			// It refers to the loader, which goes after the storage.
			if (app_storage.has_value<core::project_module_host>()) {
				app_storage.destruct<core::project_module_host>();
			}
			module_loader.shutdown_all_services(app_storage);
		} catch (const std::exception& e) {
			egLOG_ERROR("app", "error during shutdown: {}", e.what());
			result = -1;
		} catch (...) {
			egLOG_ERROR("app", "error during shutdown: unknown exception");
			result = -1;
		}
	}

	engine::shutdown_logger();
	return result;
}
