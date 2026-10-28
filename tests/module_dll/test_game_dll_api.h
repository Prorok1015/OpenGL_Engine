#pragma once
#include <string_view>

// What the host test knows of the game module in test_game_dll (a shared library loaded from disk).
namespace dll_game {

	struct dll_component {
		int value = 0;
	};

	inline constexpr std::string_view module_name = "test_game_dll";
	inline constexpr std::string_view desc_type = "dll_desc";
	inline constexpr std::string_view system_name = "dll::move";
	inline constexpr std::string_view component_name = "dll.component";

}
