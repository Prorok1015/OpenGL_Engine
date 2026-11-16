cmake_minimum_required(VERSION 3.16)
project(resource_system)
add_library(resource_system STATIC)
add_library(snk::resource_system ALIAS resource_system)

file(GLOB_RECURSE SE_RESOURCE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_RESOURCE_SOURCES})
snk_component_headers(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::engine_logger PRIVATE snk::image PUBLIC Boost::json PUBLIC Boost::asio PRIVATE snk::config_system PRIVATE snk::project)
# PUBLIC: res_vfs_resolver.h names the no-follow file primitives of platform::beneath (#414, #484, #523).
target_link_libraries(${PROJECT_NAME} PUBLIC snk::platform)

