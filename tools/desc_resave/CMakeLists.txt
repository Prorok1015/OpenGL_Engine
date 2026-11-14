cmake_minimum_required(VERSION 3.16)
project(desc_resave)

# Loads every .desc under a resources directory through the engine and writes it back in the current
# format (see main.cpp). Console tool: no window, no GPU.
add_executable(${PROJECT_NAME} main.cpp)
# config_system is in snk_engine when it is shared: its headers only, or the binary carries a second registry (#874).
snk_usage_only(_desc_resave_config snk::config_system)
target_link_libraries(${PROJECT_NAME} PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE ${_desc_resave_config} PRIVATE snk::sanitizers PRIVATE snk::project_migration)
if(WIN32 AND SNK_ENGINE_SHARED)
	# Windows has no rpath: snk_engine.dll must sit next to the executable.
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}> $<TARGET_FILE_DIR:${PROJECT_NAME}>
		COMMAND_EXPAND_LISTS)
endif()
