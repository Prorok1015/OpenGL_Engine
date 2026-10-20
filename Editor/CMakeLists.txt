cmake_minimum_required(VERSION 3.16)
project(editor)
set(WITH_CONSOLE_MODE TRUE)

if(${WITH_CONSOLE_MODE})  
	add_executable(${PROJECT_NAME} "main.cpp")
	target_compile_definitions(${PROJECT_NAME} PUBLIC ENGINE_CONSOLE_MODE=1)
else()
	add_executable(${PROJECT_NAME} WIN32 "main.cpp")
endif()

# The MCP server module: no engine dependency, linked into the editor only (see cmake/SnkMcpIsolation.cmake).
add_subdirectory("code/mcp_server")
add_subdirectory("code/editor_system")
target_link_libraries(${PROJECT_NAME} PRIVATE snk::editor_system PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE snk::sanitizers)
if(WIN32 AND SNK_ENGINE_SHARED)
	# Windows has no rpath: snk_engine.dll must sit next to the executable.
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}> $<TARGET_FILE_DIR:${PROJECT_NAME}>
		COMMAND_EXPAND_LISTS)
endif()

