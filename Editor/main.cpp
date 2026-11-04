#include <snk/application/application.h>
#include <snk/config/cfg_variables.h>
#include <snk/application/app_module_loader.hpp>
#include <snk/common/ds/ds_store.hpp>
#include <snk/core/core_module.h>
#include <snk/core/core_project_modules.h>
#include <snk/engine/engine_module.h>
#include <snk/common/logger/engine_log.h>
#include <snk/editor/editor_module.h>
#include <snk/project/prj_project.h>
#include <string_view>
#include <utility>

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
	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		if (arg == "--headless" || arg == "--mcp-stdio") {
			log_to_stderr = true;
		}
	}
	engine::init_logger(log_to_stderr ? engine::console_target::stderr_ : engine::console_target::stdout_);

	if (!cfg::initialize_configs(argc, argv)) {
		return -1;
	}

	ds::app_data_storage app_storage;

	app::modules::module_loader module_loader;
	module_loader.add_module(std::make_unique<core::core_module>());
	module_loader.add_module(std::make_unique<engine::engine_module>());
	module_loader.add_module(std::make_unique<editor::editor_module>());

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
		project_modules.with_editor_modules = true;
		// In the storage, so the editor can reload a module from a new build (#435).
		app_storage.construct<core::project_module_host>(module_loader, app_storage, std::move(project_modules)).load_all();

		app::application editor;
		result = editor.run(app_storage);
		// Read before shutdown, which removes it.
		if (app_storage.has_value<prj::held_project>()) {
			const auto& held = app_storage.require<prj::held_project>();
			if (const int code = prj::exit_code_for(held); code != 0) {
				egLOG_ERROR("app", "project '{}' was not opened: its engine version {} does not match this editor",
					held.name, prj::to_string(held.engine));
				result = code;
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
