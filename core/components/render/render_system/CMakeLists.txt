cmake_minimum_required(VERSION 3.16)
project(render)
add_library(render)

file(GLOB_RECURSE SE_RENDER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_RENDER_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE common PRIVATE window PRIVATE resource_system PRIVATE desc_system PRIVATE input_system PUBLIC drv_interface PRIVATE engine_logger)