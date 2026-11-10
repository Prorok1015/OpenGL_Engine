#pragma once
#include "../test_temp_dir.hpp"
#include "test_game_dll_api.h"
#include <snk/common/ds/ds_store.hpp>
#include <snk/platform/plt_process.h>
#include <snk/project/prj_project.h>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace test_util {

	// Whether the image is still mapped (a leftover reference, or GNU_UNIQUE symbols pinning it on Linux). Defined in
	// app_module_dll_tests.cpp, so that <windows.h> and its macros stay out of the files including this one.
	bool resident(const std::filesystem::path& file);

	// The file name core::load_project_modules looks for in <root>/build/bin; the built test library may carry a debug
	// postfix.
	inline std::string project_library_name(std::string_view module)
	{
#if defined(_WIN32)
		return std::string{ module } + ".dll";
#else
		return "lib" + std::string{ module } + ".so";
#endif
	}

	// A project in a temporary folder whose manifest names test_game_dll, with a build of it in build/bin (#435).
	struct dll_project {
		temp_dir dir;
		std::filesystem::path game;
		std::filesystem::path built;

		dll_project(std::string_view name, ds::app_data_storage& storage)
			: dir(name)
		{
			namespace fs = std::filesystem;
			fs::create_directories(dir.path() / "game" / "build" / "bin");
			game = fs::canonical(dir.path() / "game");
			built = game / "build" / "bin" / project_library_name(dll_game::module_name);
			prj::project_context project;
			project.root = game;
			project.data.name = "Game";
			project.data.modules.emplace_back();
			project.data.modules.back().name = std::string{ dll_game::module_name };
			storage.construct<prj::project_context>(std::move(project));
		}

		// What a build of the module leaves in build/bin.
		void build(const std::filesystem::path& library) const
		{
			std::filesystem::copy_file(library, built, std::filesystem::copy_options::overwrite_existing);
			BOOST_REQUIRE_MESSAGE(std::filesystem::is_regular_file(built), "not at the lookup path: " << built.string());
		}

		// A later build: a copy within the file system's time resolution keeps the old time, so it is moved on.
		void rebuild(const std::filesystem::path& library) const
		{
			build(library);
			std::filesystem::last_write_time(built, std::filesystem::last_write_time(built) + std::chrono::hours(1));
		}

		// The folder of this process's n-th copy, and the module in it.
		std::filesystem::path copy_folder(int n) const
		{
			return game / ".cache" / "modules" / std::format("{}-{}", platform::current_process_id(), n);
		}
		std::filesystem::path copy(int n) const { return copy_folder(n) / project_library_name(dll_game::module_name); }
	};

}
