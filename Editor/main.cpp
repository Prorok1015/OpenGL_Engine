#include <application.h>
#include <components_init.h>
#include "code/editor_system/edt_editor_init.h"
#include <cfg_api.h>

#ifndef _WINDOWS
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
#ifndef _WINDOWS
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

	ds::AppDataStorage app_storage;
	com::component_init(app_storage);
	com::editor_init(app_storage);

	app::Application& myApp = app::get_app_system();
	int result = myApp.run();

	com::editor_term(app_storage);
	com::component_term(app_storage);

	return result;
}
