cmake_minimum_required(VERSION 3.16)
project(application)
add_library(application)

file(GLOB_RECURSE SE_APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_APP_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} common window render input_system resource_system scene ecs game gui desc_system)

target_link_libraries(${PROJECT_NAME} Boost::json)
