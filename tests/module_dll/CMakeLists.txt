# A game module as its own shared library, loaded by unit_tests through app::modules::native_library_source
# (#220 PR-7, docs/roadmap/design/MODULE_LAYER.md). Shared engine only: the module links snk_engine like a
# game.dll would; with a static engine it would carry its own copy of every engine singleton.
add_library(test_game_dll SHARED test_game_dll.cpp)
target_link_libraries(test_game_dll PRIVATE snk::engine snk::engine_api snk::sanitizers)
if(MSVC)
	# snk_engine.dll exports only SNK_API: Boost.JSON (desc serialize/deserialize) needs its own static
	# copy in the module, as in editor_system (SPIKE-218 §3.6). On Linux the engine's copy is used.
	target_link_libraries(test_game_dll PRIVATE Boost::json)
endif()

target_sources(unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app_module_dll_tests.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/app_module_dll_tests.cpp" TARGET_DIRECTORY unit_tests
	PROPERTIES COMPILE_DEFINITIONS "SNK_TEST_GAME_DLL_PATH=\"$<TARGET_FILE:test_game_dll>\"" SKIP_PRECOMPILE_HEADERS ON)
target_link_libraries(unit_tests PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(unit_tests test_game_dll)
