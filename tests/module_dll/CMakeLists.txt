# A game module as its own shared library, loaded by unit_tests through app::modules::native_library_source
# (#220 PR-7, docs/roadmap/design/MODULE_LAYER.md). Shared engine only: the module links snk_engine like a
# game.dll would; with a static engine it would carry its own copy of every engine singleton.
#
# The other builds of the same module are what a reload swaps in (#435): v2 moves by 2, failing fails to register,
# bad_api reports the next major API version, bad_restore has a system that cannot start while a test sets a trap (#601),
# bad_layout reports a layout key other than the engine's (#614).
# The trap of bad_restore: an absolute path both the library and the test name, whatever the working directory.
set(_keeper_trap "${CMAKE_CURRENT_BINARY_DIR}/test_game_dll_keeper.trap")
foreach(_variant IN ITEMS "" v2 failing bad_api bad_restore bad_layout)
	set(_target test_game_dll)
	if(_variant)
		set(_target test_game_dll_${_variant})
	endif()
	add_library(${_target} SHARED test_game_dll.cpp)
	if(_variant)
		string(TOUPPER "${_variant}" _define)
		target_compile_definitions(${_target} PRIVATE TEST_DLL_${_define})
	endif()
	target_compile_definitions(${_target} PRIVATE "SNK_TEST_KEEPER_TRAP_PATH=\"${_keeper_trap}\"")
	target_link_libraries(${_target} PRIVATE snk::engine snk::engine_api snk::sanitizers)
	if(MSVC)
		# snk_engine.dll exports only SNK_API: Boost.JSON (desc serialize/deserialize) needs its own static
		# copy in the module, as in editor_system (SPIKE-218 §3.6). On Linux the engine's copy is used.
		target_link_libraries(${_target} PRIVATE Boost::json)
	endif()
	add_dependencies(unit_tests ${_target})
endforeach()

# #558: a module that copies a prefab_node, linked as the project template links `game`: snk::engine and snk::engine_api
# only, no Boost.JSON of its own. It is loaded and unloaded by app_module_dll_tests.
add_library(test_copy_prefab_dll SHARED test_copy_prefab_dll.cpp)
target_link_libraries(test_copy_prefab_dll PRIVATE snk::engine snk::engine_api snk::sanitizers)
add_dependencies(unit_tests test_copy_prefab_dll)

# edt_play_reload_dll_tests: Play kept across a reload of these builds (#600, HOT_RELOAD_PLAY.md 11).
set(_dll_tests "${CMAKE_CURRENT_SOURCE_DIR}/app_module_dll_tests.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/edt_play_reload_dll_tests.cpp")
target_sources(unit_tests PRIVATE ${_dll_tests})
set_source_files_properties(${_dll_tests} TARGET_DIRECTORY unit_tests
	PROPERTIES COMPILE_DEFINITIONS "SNK_TEST_GAME_DLL_PATH=\"$<TARGET_FILE:test_game_dll>\";SNK_TEST_GAME_DLL_V2_PATH=\"$<TARGET_FILE:test_game_dll_v2>\";SNK_TEST_GAME_DLL_FAILING_PATH=\"$<TARGET_FILE:test_game_dll_failing>\";SNK_TEST_GAME_DLL_BAD_API_PATH=\"$<TARGET_FILE:test_game_dll_bad_api>\";SNK_TEST_GAME_DLL_BAD_RESTORE_PATH=\"$<TARGET_FILE:test_game_dll_bad_restore>\";SNK_TEST_GAME_DLL_BAD_LAYOUT_PATH=\"$<TARGET_FILE:test_game_dll_bad_layout>\";SNK_TEST_KEEPER_TRAP_PATH=\"${_keeper_trap}\";SNK_TEST_COPY_PREFAB_DLL_PATH=\"$<TARGET_FILE:test_copy_prefab_dll>\"" SKIP_PRECOMPILE_HEADERS ON)
target_link_libraries(unit_tests PRIVATE ${CMAKE_DL_LIBS})
