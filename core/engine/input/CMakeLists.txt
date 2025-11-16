cmake_minimum_required(VERSION 3.16)
project(input_system)
add_library(input_system)

file(GLOB_RECURSE SE_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_GAME_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PUBLIC window::event_interface)
target_link_libraries(${PROJECT_NAME} PRIVATE common PRIVATE ecs PRIVATE resource_system PRIVATE engine_logger)