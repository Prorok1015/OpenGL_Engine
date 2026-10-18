cmake_minimum_required(VERSION 3.16)
project(desc_system)
add_library(desc_system STATIC)
add_library(snk::desc_system ALIAS desc_system)

file(GLOB_RECURSE SE_DESC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_DESC_SOURCES})

snk_component_headers(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::resource_system PRIVATE snk::ecs PUBLIC Boost::json PUBLIC Boost::describe PUBLIC Boost::mp11)