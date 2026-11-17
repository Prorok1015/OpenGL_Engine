cmake_minimum_required(VERSION 3.16)
project(input_system)
add_library(input_system STATIC)
add_library(snk::input_system ALIAS input_system)

file(GLOB_RECURSE SE_INPUT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_INPUT_SOURCES})

snk_component_headers(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::ecs PRIVATE snk::window PRIVATE snk::engine_logger)
# inp_events.hpp includes <snk/core_input/...>: its users need those headers, which window gave them only by chance.
target_link_libraries(${PROJECT_NAME} PUBLIC snk::core_input)