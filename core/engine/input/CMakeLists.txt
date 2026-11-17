cmake_minimum_required(VERSION 3.16)
project(input_system)
add_library(input_system STATIC)
add_library(snk::input_system ALIAS input_system)

file(GLOB_RECURSE SE_INPUT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_INPUT_SOURCES})

snk_component_headers(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PUBLIC snk::ecs PUBLIC snk::window PRIVATE snk::engine_logger)
# The public headers include ecs, window and core_input headers: their users need them (scene had them only by its own window link).
target_link_libraries(${PROJECT_NAME} PUBLIC snk::core_input)