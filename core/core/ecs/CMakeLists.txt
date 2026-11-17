cmake_minimum_required(VERSION 3.16)
project(ecs)
add_library(${PROJECT_NAME} STATIC)
add_library(snk::ecs ALIAS ${PROJECT_NAME})

file(GLOB_RECURSE SE_ECS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_ECS_SOURCES})
snk_component_headers(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common)

add_subdirectory(${THIRD_PARTY_PATH}/entt entt)

target_link_libraries(${PROJECT_NAME} PUBLIC EnTT)