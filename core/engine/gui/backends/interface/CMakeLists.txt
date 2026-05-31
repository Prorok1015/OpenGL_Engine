cmake_minimum_required(VERSION 3.16)
project(gui_backend_interface)

add_library(gui_backend_interface INTERFACE)
add_library(gui_backend_interface::gui_backend_interface ALIAS gui_backend_interface)
add_library(snk::gui_backend_interface ALIAS gui_backend_interface)

target_include_directories(gui_backend_interface INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# imgui headers come from the reusable `imgui` target (EPIC-41 US-41-6).
target_link_libraries(gui_backend_interface INTERFACE imgui)