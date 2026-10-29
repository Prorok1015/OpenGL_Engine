cmake_minimum_required(VERSION 3.16)
project(platform_system)
add_library(platform_system STATIC)
add_library(snk::platform ALIAS platform_system)

file(GLOB SE_PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(platform_system PRIVATE ${SE_PLATFORM_SOURCES})

snk_component_headers(platform_system)

# The operating system's services the engine and the editor share (EPIC-42 #158: restarting the editor).
target_link_libraries(platform_system PUBLIC snk::api PRIVATE snk::engine_logger PRIVATE $<BUILD_LOCAL_INTERFACE:snk::compiler_warnings>)
if(WIN32)
	target_link_libraries(platform_system PRIVATE shell32)
endif()
