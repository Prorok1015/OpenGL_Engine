cmake_minimum_required(VERSION 3.16)
project(application)
add_library(application STATIC)
add_library(snk::application ALIAS application)

file(GLOB_RECURSE SE_APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_APP_SOURCES})
snk_component_headers(${PROJECT_NAME})

# app.advance registers into the process-wide command registry (cmd_registry.h is in its public header).
target_link_libraries(${PROJECT_NAME} PUBLIC snk::command PRIVATE snk::common)
