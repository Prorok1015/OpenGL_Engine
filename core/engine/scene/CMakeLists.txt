cmake_minimum_required(VERSION 3.16)
project(scene)
add_library(${PROJECT_NAME} STATIC)

file(GLOB_RECURSE SE_RENDER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_RENDER_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE common PRIVATE render PRIVATE resource_system PRIVATE input_system PRIVATE window PRIVATE gui PRIVATE ecs PRIVATE engine_logger)
target_link_libraries(${PROJECT_NAME} PUBLIC desc_system)

#temporary rilation. it will move to "import_component" in the future
target_link_libraries(${PROJECT_NAME} PRIVATE assimp)