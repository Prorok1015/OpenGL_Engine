cmake_minimum_required(VERSION 3.16)
project(drv_interface)

add_library(drv_interface INTERFACE)
add_library(drv_interface::drv_interface ALIAS drv_interface)
add_library(snk::drv_interface ALIAS drv_interface)

# rnd_render_mode.h describes RENDER_MODE for the desc system, which names the topologies of a material by it.
target_link_libraries(drv_interface INTERFACE snk::common Boost::describe)

snk_component_headers(drv_interface)
