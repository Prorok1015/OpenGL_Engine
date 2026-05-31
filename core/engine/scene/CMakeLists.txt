cmake_minimum_required(VERSION 3.16)
project(scene)
add_library(${PROJECT_NAME} STATIC)
add_library(snk::scene ALIAS ${PROJECT_NAME})

file(GLOB_RECURSE SE_SCENE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_SCENE_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::render PRIVATE snk::resource_system PRIVATE snk::input_system PRIVATE snk::window PRIVATE snk::gui PRIVATE snk::ecs PRIVATE snk::engine_logger)
target_link_libraries(${PROJECT_NAME} PUBLIC snk::desc_system)

#temporary rilation. it will move to "import_component" in the future
target_link_libraries(${PROJECT_NAME} PRIVATE assimp)