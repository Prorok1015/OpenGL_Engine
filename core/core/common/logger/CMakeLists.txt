cmake_minimum_required(VERSION 3.16)
project(engine_logger)
add_library(engine_logger "engine_log.cpp")

target_include_directories(engine_logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})