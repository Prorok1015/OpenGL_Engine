# An editor module declared with SNK_DECLARE_EDITOR_MODULE, as a project's game_editor is: unit_tests opens it with
# native_library_source and checks the editor layout key it exports across the binary boundary
# (tests/edt_editor_module_dll_tests.cpp, #969). Shared engine only, like tests/editor_api_dll.
add_library(test_editor_module_dll SHARED test_editor_module_dll.cpp)
target_link_libraries(test_editor_module_dll PRIVATE snk::editor_api snk::engine snk::engine_api snk::sanitizers)
if(MSVC)
	# snk_engine.dll exports only SNK_API; see tests/module_dll/CMakeLists.txt.
	target_link_libraries(test_editor_module_dll PRIVATE Boost::json)
endif()
add_dependencies(unit_tests test_editor_module_dll)

set_source_files_properties("${CMAKE_SOURCE_DIR}/tests/edt_editor_module_dll_tests.cpp" TARGET_DIRECTORY unit_tests
	PROPERTIES COMPILE_DEFINITIONS "SNK_TEST_EDITOR_MODULE_DLL_PATH=\"$<TARGET_FILE:test_editor_module_dll>\"" SKIP_PRECOMPILE_HEADERS ON)
