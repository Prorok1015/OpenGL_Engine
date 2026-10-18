cmake_minimum_required(VERSION 3.16)
project(core_input)

add_library(${PROJECT_NAME} INTERFACE)
add_library(core::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
add_library(snk::core_input ALIAS ${PROJECT_NAME})
snk_component_headers(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME} INTERFACE snk::common)

