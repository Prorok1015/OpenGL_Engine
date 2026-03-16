cmake_minimum_required(VERSION 3.16)
project(ecs)
add_library(${PROJECT_NAME} STATIC)

file(GLOB_RECURSE SE_RENDER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_RENDER_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE common)

add_subdirectory(${THIRD_PARTY_PATH}/entt entt)

target_link_libraries(${PROJECT_NAME} PUBLIC EnTT)