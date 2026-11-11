#pragma once
#include <cstddef>
#include <string_view>

// What the host test knows of the game module in test_game_dll (a shared library loaded from disk).
namespace dll_game {

	struct dll_component {
		int value = 0;
	};

	inline constexpr std::string_view module_name = "test_game_dll";
	inline constexpr std::string_view desc_type = "dll_desc";
	inline constexpr std::string_view system_name = "dll::move";
	// Moves every animated model one clip on per frame with scn::play_animation (#563): idle -> walk -> jump (once).
	inline constexpr std::string_view animate_system_name = "dll::animate";
	// After the delivery point: a model whose clip without loop ended (scn::animation_finished) plays idle again.
	inline constexpr std::string_view back_to_idle_system_name = "dll::back_to_idle";
	inline constexpr std::string_view component_name = "dll.component";
	// A component without a C++ type (#606), registered from this schema through module_context::register_component.
	inline constexpr std::string_view schema_component_name = "dll.schema_health";
	inline constexpr const char* schema_component_json = R"json({
		"name": "dll.schema_health", "kind": "component",
		"fields": [ { "name": "hp", "kind": "f32", "default": 10 }, { "name": "target", "kind": "entity" } ]
	})json";
	// A system of the module that keeps state in its fields (#601). The bad_restore build's one cannot start while the
	// file keeper_trap_file (an absolute path in the build tree) exists: Play cannot be restored in that build.
	inline constexpr std::string_view keeper_system_name = "dll::keeper";
	inline constexpr const char* keeper_trap_file = SNK_TEST_KEEPER_TRAP_PATH;
	// When the host stored it before loading, the module waits for it with res_handle::then (#406): a callback of the
	// library's code on a block that settles after the library is gone.
	inline constexpr std::string_view waited_tag = "memory://t/dll_waited.desc";
	// Same, for a resource that fails to load: its callback is dropped without a call.
	inline constexpr std::string_view failing_tag = "memory://t/dll_failing.desc";
	// When the host stored base_tag (a text: the base's adapter), the module waits for it and for base_missing_tag
	// (never stored, fails) through res_handle::then (#416): blocks of the BASE's cache, requested from the library.
	inline constexpr std::string_view base_tag = "memory://t/dll_base.txt";
	inline constexpr std::string_view base_missing_tag = "memory://t/dll_base_missing.txt";
	// The demo's snake on the public API (#551): every frame a segment (the prefab segment_tag, stored by the host) is
	// spawned at the head with scn::spawn_prefab, and once there are snake_length the oldest goes with scn::despawn.
	inline constexpr std::string_view snake_system_name = "dll::snake";
	inline constexpr std::string_view segment_tag = "memory://t/dll_segment.prefab.desc";
	inline constexpr std::string_view segment_name = "dll_segment";
	inline constexpr std::size_t snake_length = 3;
	// A game's rounds on the public API (#564): menu -> round -> result -> menu, each a level of one world that runs one
	// of these systems and asks for the next level with scn::request_level. The number of rounds played is kept in the
	// module layer's storage, which outlives the worlds, and written into the round's world as a dll_component.
	inline constexpr std::string_view menu_system_name = "dll::menu";
	inline constexpr std::string_view round_system_name = "dll::round";
	inline constexpr std::string_view result_system_name = "dll::result";
	inline constexpr std::string_view menu_tag = "memory://t/dll_menu.desc";
	inline constexpr std::string_view round_tag = "memory://t/dll_round.desc";
	inline constexpr std::string_view result_tag = "memory://t/dll_result.desc";

}
