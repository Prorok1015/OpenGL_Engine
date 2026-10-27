# Spike #218: a game module as its own shared library, loaded by unit_tests with dlopen / LoadLibrary
# (docs/roadmap/design/SPIKE-218-entt-dll.md). Shared engine only: the module links snk_engine like a
# game.dll would; with a static engine it would carry its own copy of every engine singleton.
add_library(spike_game SHARED spike_game_module.cpp)
target_link_libraries(spike_game PRIVATE snk::engine snk::engine_api snk::sanitizers)
if(MSVC)
	# snk_engine.dll exports only SNK_API: Boost.JSON (desc serialize/deserialize) needs its own static
	# copy in the module, as in editor_system. On Linux the engine's copy is visible and is used.
	target_link_libraries(spike_game PRIVATE Boost::json)
endif()
# The ENTT_* experiments of the spike change definitions on this target only; the shared PCH was
# built without them.
set_target_properties(spike_game PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)

target_sources(unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app_module_dll_tests.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/app_module_dll_tests.cpp" TARGET_DIRECTORY unit_tests
	PROPERTIES COMPILE_DEFINITIONS "SNK_SPIKE_GAME_PATH=\"$<TARGET_FILE:spike_game>\"" SKIP_PRECOMPILE_HEADERS ON)
target_link_libraries(unit_tests PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(unit_tests spike_game)
# EXPERIMENT (spike #218, reverted next): entt::type_index imported from another binary
# (ENTT_API = dllimport on Windows, default visibility on Linux).
target_compile_definitions(spike_game PRIVATE ENTT_API_IMPORT)
