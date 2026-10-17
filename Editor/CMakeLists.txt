cmake_minimum_required(VERSION 3.16)
project(editor)
set(SE_RESOURCE_ABSOLUTE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/")
set(WITH_CONSOLE_MODE TRUE)

if(${WITH_CONSOLE_MODE})  
	add_executable(${PROJECT_NAME} "main.cpp")
	target_compile_definitions(${PROJECT_NAME} PUBLIC ENGINE_CONSOLE_MODE=1)
else()
	add_executable(${PROJECT_NAME} WIN32 "main.cpp")
endif()

add_subdirectory("code/editor_system")
target_link_libraries(${PROJECT_NAME} PRIVATE snk::editor_system PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE snk::config_system PRIVATE snk::sanitizers)
# main.cpp bootstraps the module system via the per-layer module-entry headers (core_module.h,
# engine_module.h) which live at the aggregator roots — exposed only to this bootstrap exe so the
# api targets stay component-scoped (EPIC-49 US-49-1).
target_include_directories(${PROJECT_NAME} PRIVATE ${ENGINE_CORE_PATH}/core ${ENGINE_CORE_PATH}/engine)

