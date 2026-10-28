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
	// When the host stored it before loading, the module waits for it with res_handle::then (#406): a callback of the
	// library's code on a block that settles after the library is gone.
	inline constexpr std::string_view waited_tag = "memory://t/dll_waited.desc";
	// Same, for a resource that fails to load: its callback is dropped without a call.
	inline constexpr std::string_view failing_tag = "memory://t/dll_failing.desc";
	// When the host stored base_tag (a text: the base's adapter), the module waits for it and for base_missing_tag
	// (never stored, fails) through res_handle::then (#416): blocks of the BASE's cache, requested from the library.
	inline constexpr std::string_view base_tag = "memory://t/dll_base.txt";
	inline constexpr std::string_view base_missing_tag = "memory://t/dll_base_missing.txt";

}
