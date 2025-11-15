cmake_minimum_required(VERSION 3.16)
project(drv_opengl)
add_library(drv_opengl)
target_compile_definitions(drv_opengl PRIVATE PROJECT_SOURCE_PATH="${SE_PROJECT_SOURCE_PATH}") 

file(GLOB_RECURSE SE_DRV_GL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_DRV_GL_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(${THIRD_PARTY_PATH}/glad glad)
target_link_libraries(${PROJECT_NAME} PRIVATE common PUBLIC glad PRIVATE drv_interface PRIVATE engine_logger)
