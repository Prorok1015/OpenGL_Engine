#include <snk/application/application.h>
#include <snk/config/cfg_variables.h>
#include <snk/application/app_module_loader.hpp>
#include <snk/common/ds/ds_store.hpp>
#include <snk/core/core_module.h>
#include <snk/core/core_project_modules.h>
#include <snk/engine/engine_module.h>
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_editor_extension.h>
#include <snk/editor/editor_module.h>
#include <snk/game/gs_module_layout.h>
#include <edt_decode_runner.h>
#include <snk/common/fs_paths.h>
#include <snk/platform/plt_process.h>
#include <snk/platform/plt_stdio.h>
#include <snk/project/prj_project.h>
#include <snk/project_migration/pmg_asset_steps.h>
#include <snk/project_migration/pmg_command.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_scene_layer.h>
#include <snk/desc/desc_system.h>
#include <string>
#include <string_view>
#include <utility>

namespace
{
	// Set by --migrate / --migrate-rollback (cfg_api.cpp, EPIC-58 S6).
	CFG_VAR_DEF_STRING(cfg_migration_command, "migration/command", "");
	CFG_VAR_DEF_STRING(cfg_migration_rollback_id, "migration/rollback_id", "");
	CFG_VAR_DEF_BOOL(cfg_migration_dry_run, "migration/dry_run", false);
	CFG_VAR_DEF_BOOL(cfg_migration_force, "migration/force", false);
	CFG_VAR_DEF_STRING(cfg_migration_report, "migration/report", "");

	// With the game layer of the project's module: its types decide what is unknown and what is outdated.
	int run_migration_command(ds::app_data_storage& storage)
	{
		const auto& project = storage.require<prj::project_context>();
		scn::scene_layer* layer = storage.require<scn::context_manager>().game_layer();
		desc::desc_system& descs = layer ? layer->descs() : storage.require<desc::desc_system>();
		res::resource_system& resources = layer ? layer->resources() : storage.require<res::resource_system>();
		prj::migration::project_step_registry steps;
		prj::migration::register_project_steps(steps);
		const prj::migration::command_context context{ project.root, descs, resources, &steps, {}, {}, {}, {} };
		if (*cfg_migration_command == "rollback") {
			return prj::migration::run_migrate_rollback(*cfg_migration_rollback_id, context);
		}
		prj::migration::migrate_command command;
		command.dry_run = cfg_migration_dry_run;
		command.force = cfg_migration_force;
		command.report = fs_paths::path_from_utf8(*cfg_migration_report);
		return prj::migration::run_migrate(command, context);
	}
}

#if defined(_WIN32) && !defined(ENGINE_CONSOLE_MODE)
#include <Windows.h>

// Add SAL annotations to match the Windows header definition
int WINAPI WinMain(
    _In_ HINSTANCE hInstance,         // The instance
    _In_opt_ HINSTANCE hPrevInstance, // Previous instance
    _In_ LPSTR lpCmdLine,             // Command Line Parameters
    _In_ int nShowCmd                 // Window Show State
)
#else
int main(int argc, char* argv[])
#endif
{
#if defined(_WIN32) && !defined(ENGINE_CONSOLE_MODE)
	int argc;
	LPCWSTR lpArgvW = GetCommandLineW();
	LPWSTR* szArglist = CommandLineToArgvW(lpArgvW, &argc);
	std::vector<std::string> args;
	for (int i = 0; i < argc; ++i) {
		int size_needed = WideCharToMultiByte(CP_UTF8, 0, szArglist[i], -1, NULL, 0, NULL, NULL);
		std::string arg(size_needed, 0);
		WideCharToMultiByte(CP_UTF8, 0, szArglist[i], -1, arg.data(), size_needed, NULL, NULL);
		args.push_back(arg);
	}
	LocalFree(szArglist);

	auto argvu = std::make_unique<char*[]>(argc);
	for (int i = 0; i < argc; ++i) {
		argvu[i] = args[i].data();
	}
	char** argv = argvu.get();
#endif

	// Before the configs are parsed, so the first line already goes there: stdout belongs to a protocol (or to
	// nobody) when the editor runs without a display.
	bool log_to_stderr = false;
	bool mcp_stdio = false;
	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		if (arg == "--headless" || arg == "--mcp-stdio") {
			log_to_stderr = true;
		}
		if (arg == "--mcp-stdio") {
			mcp_stdio = true;
		}
	}
	// MCP over stdio: only the protocol may reach the real stdout, so whatever else writes there (a library's printf,
	// --help) lands on stderr from here on.
	const int protocol_fd = mcp_stdio ? platform::take_stdout_for_protocol() : -1;
	engine::init_logger(log_to_stderr ? engine::console_target::stderr_ : engine::console_target::stdout_);
	if (mcp_stdio && protocol_fd < 0) {
		egLOG_ERROR("app", "--mcp-stdio: stdout could not be taken for the protocol");
	}

	// The package's layout (bin/, config/editor.cfg): from the executable's folder, whatever the working directory (#160).
	// An unknown executable path leaves no default rather than one relative to the working directory.
	const std::filesystem::path executable = platform::current_executable();
	const std::filesystem::path default_config =
		executable.empty() ? std::filesystem::path{} : executable.parent_path() / ".." / "config" / "editor.cfg";
	try {
		if (!cfg::initialize_configs(argc, argv, default_config)) {
			return -1;
		}
	} catch (const std::exception& e) {
		egLOG_ERROR("app", "{}", cfg::startup_error_message(e));
		return 2;
	}
	if (cfg::loaded_config_path().empty()) {
		egLOG_ERROR("app", "{}", cfg::no_config_message(default_config));
		return 2;
	}
	egLOG_INFO("app", "config: {}", fs_paths::generic_utf8_of(cfg::loaded_config_path()));

	// What the editor defaults after the configs: the window layout goes to the user's folder, not to the current one.
	editor::apply_default_settings();

	ds::app_data_storage app_storage;
	if (protocol_fd >= 0) {
		app_storage.construct<platform::protocol_stdout>(protocol_fd);
	}

	// A migration runs without the editor: nothing of it starts, so nothing of it writes into the project (S6).
	const bool migration = !cfg_migration_command->empty();

	app::modules::module_loader module_loader;
	module_loader.add_module(std::make_unique<core::core_module>());
	module_loader.add_module(std::make_unique<engine::engine_module>());
	if (!migration) {
		module_loader.add_module(std::make_unique<editor::editor_module>());
	}

	// Exception perimeter: nothing may escape main(). initialize_all_services rolls back
	// the modules it already initialized if one throws, so shutdown below only runs
	// once initialization has fully succeeded.
	int result = -1;
	bool initialized = false;
	try {
		module_loader.register_all_services(app_storage);
		module_loader.initialize_all_services(app_storage);
		initialized = true;
		// After the host's own modules: initialize_all_services must not see the loader grow.
		core::project_modules_options project_modules;
		project_modules.with_editor_modules = !migration;
		// A game.dll built against other layouts of the engine's types is refused, not loaded (#614).
		project_modules.layout_key = gs::engine_module_layout_key();
		project_modules.check_editor_module = [](const app::modules::module_entry& entry) {
			return edt::check_editor_api_version(entry.editor_api_version);
		};
		project_modules.editor_unload_refusal = [&app_storage]() -> std::string {
			return app_storage.has_value<edt::editor_context>() ? app_storage.require<edt::editor_context>().module_unload_refusal() : std::string{};
		};
		// In the storage, so the editor can reload a module from a new build (#435).
		auto& project_host = app_storage.construct<core::project_module_host>(module_loader, app_storage, std::move(project_modules));
		project_host.load_all();
		if (migration) {
			result = run_migration_command(app_storage);
		} else {
			// After `game`: a game_editor registers into the editor on top of it (#588).
			project_host.load_editor_modules();
			// The run folders of model decoders whose editor crashed (#647).
			try {
				edt::decode_runner::remove_stale_folders();
			} catch (const std::exception& e) {
				egLOG_WARN("app", "cleanup of the folders of ended model decoders failed: {}", e.what());
			}

			app::application editor;
			result = editor.run(app_storage);
			// Read before shutdown, which removes it.
			if (app_storage.has_value<prj::held_project>()) {
				const auto& held = app_storage.require<prj::held_project>();
				if (const int code = prj::exit_code_for(held); code != 0) {
					if (held.answer == prj::held_project::decision::no_project) {
						egLOG_ERROR("app", "no project: '{}' was not opened (engine {}) and this run needs a project",
							held.name, prj::to_string(held.engine));
					} else {
						egLOG_ERROR("app", "project '{}' was not opened: its engine version {} does not match this editor",
							held.name, prj::to_string(held.engine));
					}
					result = code;
				}
			}
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
