# A module that registers into the editor through the shared editor API library (snk_editor_api, #587), as a game_editor
# module does: it links the engine and the API library only, not editor_system. Loaded by unit_tests (tests/edt_editor_api_module_tests.cpp).
# Shared engine only, like tests/module_dll.
add_library(test_editor_api_dll SHARED test_editor_api_dll.cpp)
target_link_libraries(test_editor_api_dll PRIVATE snk::editor_api snk::engine snk::engine_api snk::sanitizers)
if(MSVC)
	# snk_engine.dll exports only SNK_API; see tests/module_dll/CMakeLists.txt.
	target_link_libraries(test_editor_api_dll PRIVATE Boost::json)
endif()
add_dependencies(unit_tests test_editor_api_dll)

set_source_files_properties("${CMAKE_SOURCE_DIR}/tests/edt_editor_api_module_tests.cpp" TARGET_DIRECTORY unit_tests
	PROPERTIES COMPILE_DEFINITIONS "SNK_TEST_EDITOR_API_DLL_PATH=\"$<TARGET_FILE:test_editor_api_dll>\"" SKIP_PRECOMPILE_HEADERS ON)
