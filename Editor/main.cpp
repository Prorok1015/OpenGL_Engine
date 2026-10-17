#include <application.h>
#include <cfg_api.h>
#include <app_module_loader.hpp>
#include <ds/ds_store.hpp>
#include <core_module.h>
#include <engine_module.h>
#include <engine_log.h>
#include "editor_module.h"

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

	engine::init_logger();

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

		app::application editor;
		result = editor.run(app_storage);
	} catch (const std::exception& e) {
		egLOG_ERROR("app", "fatal error: {}", e.what());
		result = -1;
	} catch (...) {
		egLOG_ERROR("app", "fatal error: unknown exception");
		result = -1;
	}

	if (initialized) {
		try {
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
