cmake_minimum_required(VERSION 3.16)
project(game)
add_library(game STATIC)

file(GLOB_RECURSE SE_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_GAME_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE common PRIVATE render PRIVATE window PRIVATE desc_system PRIVATE ecs PUBLIC scene PUBLIC gui PRIVATE input_system PRIVATE resource_system)