cmake_minimum_required(VERSION 3.16)
project(engine_logger)

# spdlog SPDLOG_USE_STD_FORMAT option lives in cmake/ThirdParty.cmake (EPIC-41 US-41-1).
add_subdirectory(${THIRD_PARTY_PATH}/spdlog ${CMAKE_CURRENT_BINARY_DIR}/spdlog)

add_library(engine_logger STATIC "engine_log.cpp")
add_library(snk::engine_logger ALIAS engine_logger)
target_link_libraries(engine_logger PUBLIC snk::api)
target_include_directories(engine_logger PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include_base")
target_link_libraries(engine_logger PRIVATE spdlog::spdlog_header_only $<BUILD_LOCAL_INTERFACE:snk::compiler_warnings>)
