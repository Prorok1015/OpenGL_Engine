cmake_minimum_required(VERSION 3.16)
project(input_system)
add_library(input_system STATIC)
add_library(snk::input_system ALIAS input_system)

file(GLOB_RECURSE SE_INPUT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_INPUT_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::ecs PRIVATE snk::window PRIVATE snk::resource_system PRIVATE snk::engine_logger)