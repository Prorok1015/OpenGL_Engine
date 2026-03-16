cmake_minimum_required(VERSION 3.16)
project(gui)
add_library(gui)
file(GLOB_RECURSE SE_DEBUG_UI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_DEBUG_UI_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE common PRIVATE window PRIVATE desc_system PRIVATE resource_system PRIVATE render PRIVATE gui_backend_interface)