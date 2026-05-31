cmake_minimum_required(VERSION 3.16)
project(window)
add_library(window STATIC)
add_library(snk::window ALIAS window)

file(GLOB_RECURSE SE_WINDOW_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_WINDOW_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

#libs
# GLFW_* / BUILD_SHARED_LIBS options live in cmake/ThirdParty.cmake (EPIC-41 US-41-1).
add_subdirectory(${THIRD_PARTY_PATH}/glfw glfw)
target_compile_definitions(window PUBLIC GLFW_INCLUDE_NONE=1)

target_link_libraries(${PROJECT_NAME} PUBLIC snk::core_input)
target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE glfw PRIVATE glad PUBLIC snk::drv_interface PRIVATE snk::drv_opengl PUBLIC snk::gui_backend_interface PRIVATE snk::gui_gl_backend PRIVATE snk::engine_logger)

