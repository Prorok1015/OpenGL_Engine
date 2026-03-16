cmake_minimum_required(VERSION 3.16)
project(editor_system)
add_library(${PROJECT_NAME})

file(GLOB_RECURSE SE_EDT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_EDT_SOURCES})
target_link_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${PROJECT_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/panels")

add_subdirectory(${ENGINE_CORE_PATH} "core")
target_link_libraries(${PROJECT_NAME} PUBLIC engine)
