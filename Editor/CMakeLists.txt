cmake_minimum_required(VERSION 3.16)
project(editor)
set(WITH_CONSOLE_MODE TRUE)

# The MCP server module: no engine dependency, linked into the editor only (see cmake/SnkMcpIsolation.cmake).
add_subdirectory("code/mcp_server")
add_subdirectory("code/editor_system")
# The model importer and its guards (editor_system links it; the decoder process of #647 will too).
add_subdirectory("code/model_import")

# The editor always runs on the shared engine (R11, #223). A static build keeps editor_system for the
# unit tests, but there is no static editor to build or ship by accident.
if(NOT SNK_ENGINE_SHARED)
	message(STATUS "Static engine (SNK_ENGINE_SHARED=OFF): the editor executable is not built (#223)")
	return()
endif()

if(${WITH_CONSOLE_MODE})  
	add_executable(${PROJECT_NAME} "main.cpp")
	target_compile_definitions(${PROJECT_NAME} PUBLIC ENGINE_CONSOLE_MODE=1)
else()
	add_executable(${PROJECT_NAME} WIN32 "main.cpp")
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE snk::editor_system PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE snk::sanitizers
	PRIVATE snk::project_migration)
if(WIN32)
	# Windows has no rpath: snk_engine.dll must sit next to the executable.
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}> $<TARGET_FILE_DIR:${PROJECT_NAME}>
		COMMAND_EXPAND_LISTS)
endif()

# The editor in the release package (component `editor`, #223), next to the SDK (component `snake_sdk`):
#   bin/editor.exe (+ PDB)  config/editor.cfg  data/ (editor icons, templates)  res_core/ (engine base resources)
#   projects/sandbox/ (the test project)  README.txt  LICENSE
# editor.cfg resolves its paths relative to itself, so the packaged copy points one level up instead of into the repository.
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin COMPONENT editor)
if(UNIX AND NOT APPLE)
	# The installed bin/editor finds libsnk_engine.so in <prefix>/lib without LD_LIBRARY_PATH (#722).
	set_target_properties(${PROJECT_NAME} PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()
if(MSVC)
	install(FILES $<TARGET_PDB_FILE:${PROJECT_NAME}> DESTINATION bin OPTIONAL COMPONENT editor)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/config/editor.cfg")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/editor.cfg" _edt_cfg)
string(REPLACE "base_path: ../../core/res/" "base_path: ../res_core/" _edt_cfg "${_edt_cfg}")
string(REPLACE "path: ../../projects/sandbox/res/" "path: ../projects/sandbox/res/" _edt_cfg "${_edt_cfg}")
foreach(_edt_expected "base_path: ../res_core/" " path: ../projects/sandbox/res/" "data_path: ../data/")
	string(FIND "${_edt_cfg}" "${_edt_expected}" _edt_at)
	if(_edt_at EQUAL -1)
		message(FATAL_ERROR "Editor/config/editor.cfg changed: the packaged copy has no '${_edt_expected}'")
	endif()
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/package/editor.cfg" "${_edt_cfg}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/package/editor.cfg" DESTINATION config COMPONENT editor)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/" DESTINATION data COMPONENT editor)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/core/res/" DESTINATION res_core COMPONENT editor)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/projects/sandbox/" DESTINATION projects/sandbox COMPONENT editor
	PATTERN ".cache" EXCLUDE
	PATTERN ".snk" EXCLUDE)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/package/README.txt" "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION . COMPONENT editor)

