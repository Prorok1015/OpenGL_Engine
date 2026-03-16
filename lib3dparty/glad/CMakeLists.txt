cmake_minimum_required(VERSION 3.16)
project(glad)

add_library(glad)

target_sources(glad PRIVATE glad.c)

target_include_directories(glad PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})