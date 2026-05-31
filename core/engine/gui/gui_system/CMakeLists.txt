cmake_minimum_required(VERSION 3.16)
project(gui)
add_library(gui STATIC)
add_library(snk::gui ALIAS gui)
file(GLOB_RECURSE SE_GUI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_GUI_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::window PRIVATE snk::desc_system PRIVATE snk::resource_system PRIVATE snk::render PRIVATE snk::gui_backend_interface)