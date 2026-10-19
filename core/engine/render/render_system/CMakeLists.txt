cmake_minimum_required(VERSION 3.16)
project(render)
add_library(render STATIC)
add_library(snk::render ALIAS render)

file(GLOB_RECURSE SE_RENDER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_RENDER_SOURCES})
snk_component_headers(${PROJECT_NAME})
# rnd_render_service_init.cpp includes the internal rnd_gl_swapchain.h of the GL driver.
target_include_directories(${PROJECT_NAME} PRIVATE "$<TARGET_PROPERTY:drv_opengl,SOURCE_DIR>" "$<TARGET_PROPERTY:drv_null,SOURCE_DIR>" "$<TARGET_PROPERTY:drv_validation,SOURCE_DIR>")

# rnd_material_compiler consumes the POD scn::material_desc header (EPIC-49 US-49-5). material_desc
# is header-only data (its methods live in scene's .cpp and the compiler calls none of them), so this
# is a header dependency only — render does NOT link scene (scene links render, that would be a cycle).
# snk::scene_material_desc_api (defined next to scene) exposes scene's public include dir without code.
# Stays until the material compiler stops taking scn::material_desc (#236).
target_link_libraries(${PROJECT_NAME} PUBLIC snk::scene_material_desc_api)


target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::window PRIVATE snk::resource_system PRIVATE snk::desc_system PRIVATE snk::input_system PUBLIC snk::drv_interface PRIVATE snk::drv_opengl PRIVATE snk::drv_null PRIVATE snk::drv_validation PRIVATE snk::config_system PRIVATE snk::engine_logger PRIVATE snk::command PRIVATE snk::image)