cmake_minimum_required(VERSION 3.16)
project(resource_system)
add_library(resource_system)

file(GLOB_RECURSE SE_RESOURCE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_RESOURCE_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE common PRIVATE assimp PRIVATE engine_logger PRIVATE image)

target_compile_definitions(${PROJECT_NAME} PUBLIC RESOURCE_PATH="${SE_RESOURCE_ABSOLUTE_PATH}") 

#assimp
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY_PATH}/assimp assimp)
