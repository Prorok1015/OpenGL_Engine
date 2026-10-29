# A game module as its own shared library, loaded by unit_tests through app::modules::native_library_source
# (#220 PR-7, docs/roadmap/design/MODULE_LAYER.md). Shared engine only: the module links snk_engine like a
# game.dll would; with a static engine it would carry its own copy of every engine singleton.
#
# The other builds of the same module are what a reload swaps in (#435): v2 moves by 2, failing fails to register,
# bad_api reports the next major API version.
foreach(_variant IN ITEMS "" v2 failing bad_api)
	set(_target test_game_dll)
	if(_variant)
		set(_target test_game_dll_${_variant})
	endif()
	add_library(${_target} SHARED test_game_dll.cpp)
	if(_variant)
		string(TOUPPER "${_variant}" _define)
		target_compile_definitions(${_target} PRIVATE TEST_DLL_${_define})
	endif()
	target_link_libraries(${_target} PRIVATE snk::engine snk::engine_api snk::sanitizers)
	if(MSVC)
		# snk_engine.dll exports only SNK_API: Boost.JSON (desc serialize/deserialize) needs its own static
		# copy in the module, as in editor_system (SPIKE-218 §3.6). On Linux the engine's copy is used.
		target_link_libraries(${_target} PRIVATE Boost::json)
	endif()
	add_dependencies(unit_tests ${_target})
endforeach()

target_sources(unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app_module_dll_tests.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/app_module_dll_tests.cpp" TARGET_DIRECTORY unit_tests
	PROPERTIES COMPILE_DEFINITIONS "SNK_TEST_GAME_DLL_PATH=\"$<TARGET_FILE:test_game_dll>\";SNK_TEST_GAME_DLL_V2_PATH=\"$<TARGET_FILE:test_game_dll_v2>\";SNK_TEST_GAME_DLL_FAILING_PATH=\"$<TARGET_FILE:test_game_dll_failing>\";SNK_TEST_GAME_DLL_BAD_API_PATH=\"$<TARGET_FILE:test_game_dll_bad_api>\"" SKIP_PRECOMPILE_HEADERS ON)
target_link_libraries(unit_tests PRIVATE ${CMAKE_DL_LIBS})
