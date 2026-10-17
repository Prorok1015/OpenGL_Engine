cmake_minimum_required(VERSION 3.16)
project(core_input)

add_library(${PROJECT_NAME} INTERFACE)
add_library(core::${PROJECT_NAME} ALIAS ${PROJECT_NAME})
add_library(snk::core_input ALIAS ${PROJECT_NAME})
target_include_directories(${PROJECT_NAME} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} INTERFACE snk::common)

