cmake_minimum_required(VERSION 3.16)
project(render)
add_library(render STATIC)
add_library(snk::render ALIAS render)

file(GLOB_RECURSE SE_RENDER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_RENDER_SOURCES})
snk_component_headers(${PROJECT_NAME})
# rnd_render_service_init.cpp includes the internal rnd_gl_swapchain.h of the GL driver.
target_include_directories(${PROJECT_NAME} PRIVATE "$<TARGET_PROPERTY:drv_opengl,SOURCE_DIR>" "$<TARGET_PROPERTY:drv_null,SOURCE_DIR>" "$<TARGET_PROPERTY:drv_validation,SOURCE_DIR>")

# rnd_material_desc.h (the material description the compiler takes) is built on the desc system, so it is a public dependency.
target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::window PRIVATE snk::resource_system PUBLIC snk::desc_system PRIVATE snk::input_system PUBLIC snk::drv_interface PRIVATE snk::drv_opengl PRIVATE snk::drv_null PRIVATE snk::drv_validation PRIVATE snk::config_system PRIVATE snk::engine_logger PRIVATE snk::command PRIVATE snk::image)