cmake_minimum_required(VERSION 3.16)
project(engine_logger)

# spdlog: use std::format (C++20), no fmtlib dependency
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY_PATH}/spdlog ${CMAKE_CURRENT_BINARY_DIR}/spdlog)

add_library(engine_logger "engine_log.cpp")
target_include_directories(engine_logger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine_logger PRIVATE spdlog::spdlog_header_only)
