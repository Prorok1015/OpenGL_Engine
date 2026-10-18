cmake_minimum_required(VERSION 3.16)
project(render)
add_library(render STATIC)
add_library(snk::render ALIAS render)

file(GLOB_RECURSE SE_RENDER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_RENDER_SOURCES})
snk_component_headers(${PROJECT_NAME})

# rnd_material_compiler consumes the POD scn::material_desc header (EPIC-49 US-49-5). material_desc
# is header-only data (its methods live in scene's .cpp and the compiler calls none of them), so this
# is a header dependency only — render does NOT link scene (scene links render, that would be a cycle).
# snk::scene_material_desc_api (defined next to scene) exposes the scene directory without code.
# Transitional until the scene headers move to scene/include (#109, PR-4).
target_link_libraries(${PROJECT_NAME} PUBLIC snk::scene_material_desc_api)


target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::window PRIVATE snk::resource_system PRIVATE snk::desc_system PRIVATE snk::input_system PUBLIC snk::drv_interface PRIVATE snk::drv_opengl PRIVATE snk::engine_logger)