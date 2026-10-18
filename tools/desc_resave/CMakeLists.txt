cmake_minimum_required(VERSION 3.16)
project(desc_resave)

# Loads every .desc under a resources directory through the engine and writes it back in the current
# format (see main.cpp). Console tool: no window, no GPU.
add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE snk::config_system PRIVATE snk::sanitizers)
if(WIN32 AND SNK_ENGINE_SHARED)
	# Windows has no rpath: snk_engine.dll must sit next to the executable.
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}> $<TARGET_FILE_DIR:${PROJECT_NAME}>
		COMMAND_EXPAND_LISTS)
endif()
