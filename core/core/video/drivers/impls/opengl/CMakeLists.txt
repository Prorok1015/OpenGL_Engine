cmake_minimum_required(VERSION 3.16)
project(drv_opengl)
add_library(drv_opengl STATIC)
add_library(snk::drv_opengl ALIAS drv_opengl)

file(GLOB_RECURSE SE_DRV_GL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_DRV_GL_SOURCES})
# Public header: include/snk/driver_gl/rnd_gl_swapchain.h (the swapchain render's backend selection creates;
# it includes only the driver interface and wnd_gl_surface.h, no GL). Everything else (rnd_gl_*, open_gl_specific.h)
# is internal: the source dir is private to the target (#109).
snk_component_headers(${PROJECT_NAME} SDK_INTERNAL)

add_subdirectory(${THIRD_PARTY_PATH}/glad glad)
target_link_libraries(${PROJECT_NAME} PUBLIC glad)
target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::drv_interface PRIVATE snk::engine_logger)
# rnd_gl_swapchain.h includes wnd_gl_surface.h: the driver depends on the gl_surface interface, not on
# the windowing library (no GLFW/core_input leaking into the GL backends).
target_link_libraries(${PROJECT_NAME} PUBLIC snk::window_gl_surface)
