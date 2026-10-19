cmake_minimum_required(VERSION 3.16)
project(command)
add_library(command STATIC)
add_library(snk::command ALIAS command)

file(GLOB SE_COMMAND_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(command PRIVATE ${SE_COMMAND_SOURCES})

snk_component_headers(command)

# Only common (logger, assert, ds::task_system) and Boost.JSON: a game without the editor registers its commands here too.
# PUBLIC common: cmd_main_thread_queue.h includes ds_task_system.hpp.
target_link_libraries(command PUBLIC snk::common PUBLIC Boost::json)
