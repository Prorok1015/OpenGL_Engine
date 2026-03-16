cmake_minimum_required(VERSION 3.16)

project(stb_image)

add_library(stb_image INTERFACE)
add_library(stb_image::stb_image ALIAS stb_image)

target_include_directories(stb_image INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(stb_image INTERFACE STB_IMAGE_IMPLEMENTATION=1 INTERFACE STB_IMAGE_WRITE_IMPLEMENTATION=1) 