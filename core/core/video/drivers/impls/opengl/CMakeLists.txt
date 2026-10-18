cmake_minimum_required(VERSION 3.16)
project(drv_opengl)
add_library(drv_opengl STATIC)
add_library(snk::drv_opengl ALIAS drv_opengl)

file(GLOB_RECURSE SE_DRV_GL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_DRV_GL_SOURCES})
snk_component_headers(${PROJECT_NAME})

add_subdirectory(${THIRD_PARTY_PATH}/glad glad)
target_link_libraries(${PROJECT_NAME} PUBLIC glad)
target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::drv_interface PRIVATE snk::engine_logger)
# rnd_gl_swapchain.h includes wnd_gl_surface.h; the window component never links back to the driver.
target_link_libraries(${PROJECT_NAME} PUBLIC snk::window)
