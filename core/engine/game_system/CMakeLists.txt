cmake_minimum_required(VERSION 3.16)
project(game)
add_library(game STATIC)
add_library(snk::game ALIAS game)

file(GLOB_RECURSE SE_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_GAME_SOURCES})

snk_component_headers(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::application PRIVATE snk::command PRIVATE snk::config_system PRIVATE snk::common PRIVATE snk::render PRIVATE snk::window PRIVATE snk::desc_system PRIVATE snk::ecs PUBLIC snk::scene PUBLIC snk::gui PRIVATE snk::input_system PRIVATE snk::resource_system)