#pragma once
#include <snk/application/app_module_entry.h>
#include <map>
#include <string>

// The shipping host as a library (#591, #592, DISTRIBUTION.md): the static engine, the project's start level, no
// editor. A game executable is a main() that calls run() with the game modules compiled into it; snk_add_game_host()
// (cmake/SnkGameHost.cmake) generates that main.
namespace snk::game_host
{
	// Above prj::k_exit_project_error (2) and prj::k_exit_engine_version (3).
	inline constexpr int k_exit_no_start_level = 4;
	inline constexpr int k_exit_level_failed = 5;
	// A module the project requires is not compiled into this host, or it did not load.
	inline constexpr int k_exit_missing_module = 6;

	// `compiled_in`: the game modules linked into the executable, by their manifest name. Returns the process exit code.
	int run(int argc, char* argv[], std::map<std::string, app::modules::module_entry> compiled_in);
}
