cmake_minimum_required(VERSION 3.16)
project(config_system)
add_library(config_system STATIC)
add_library(snk::config_system ALIAS config_system)

file(GLOB_RECURSE SE_CONFIG_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_CONFIG_SOURCES})

snk_component_headers(${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME} PUBLIC snk::api PRIVATE $<BUILD_LOCAL_INTERFACE:snk::compiler_warnings>)

add_subdirectory(${THIRD_PARTY_PATH}/yaml yaml-cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE yaml-cpp PRIVATE Boost::program_options PRIVATE snk::common)