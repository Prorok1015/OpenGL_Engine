#include <application.h>
#include <cfg_api.h>
#include <eng_module_loader.hpp>
#include <ds_store.hpp>
#include <core_module.h>
#include <engine_module.h>
#include "code/editor_system/editor_module.h"

#ifndef ENGINE_CONSOLE_MODE
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
#ifndef ENGINE_CONSOLE_MODE
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

	if (!cfg::initialize_configs(argc, argv)) {
		return -1;
	}

	ds::app_data_storage app_storage;

	modules::module_loader module_loader;
	module_loader.add_module(std::make_unique<core::core_module>());
	module_loader.add_module(std::make_unique<engine::engine_module>());
	module_loader.add_module(std::make_unique<editor::editor_module>());

	module_loader.register_all_services(app_storage);
	module_loader.initialize_all_services(app_storage);

	app::application& myApp = app_storage.require<app::application>();
	int result = myApp.run(app_storage);

	module_loader.shutdown_all_services(app_storage);

	return result;
}
