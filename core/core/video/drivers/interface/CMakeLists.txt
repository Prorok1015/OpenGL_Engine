cmake_minimum_required(VERSION 3.16)
project(drv_interface)

add_library(drv_interface INTERFACE)
add_library(drv_interface::drv_interface ALIAS drv_interface)

target_link_libraries(drv_interface INTERFACE common)

target_include_directories(drv_interface INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
