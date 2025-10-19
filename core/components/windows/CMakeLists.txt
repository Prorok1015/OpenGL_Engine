cmake_minimum_required(VERSION 3.16)
project(window)
add_library(window STATIC)

file(GLOB_RECURSE SE_WINDOW_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_WINDOW_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

#libs
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY_PATH}/glfw glfw)
target_compile_definitions(window PUBLIC GLFW_INCLUDE_NONE=1)

target_link_libraries(${PROJECT_NAME} PRIVATE common PRIVATE glfw PRIVATE glad PUBLIC drv_interface PRIVATE drv_opengl PUBLIC gui_backend_interface PRIVATE gui_gl_backend PRIVATE resource_system PRIVATE input_system PRIVATE engine_logger)

