cmake_minimum_required(VERSION 3.16)
project(scene)
add_library(${PROJECT_NAME} STATIC)
add_library(snk::scene ALIAS ${PROJECT_NAME})

file(GLOB_RECURSE SE_SCENE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_SCENE_SOURCES})
snk_component_headers(${PROJECT_NAME})

# Header-only view of scene for render (rnd_material_compiler.h -> scn_material_desc.h); render cannot
# link scene itself (cycle). Stays until the material compiler stops taking scn::material_desc (#236).
add_library(scene_material_desc_api INTERFACE)
add_library(snk::scene_material_desc_api ALIAS scene_material_desc_api)
target_include_directories(scene_material_desc_api INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

# scene does NOT use gui (verified: zero gui includes / gui:: usage) — the link was dead and is a
# layer violation (scene must not depend on gui). Removed (EPIC-49 US-49-9). gui takes the rendered
# scene texture from the renderer via the existing extractor/frame_context seam, not the reverse.
target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::render PRIVATE snk::resource_system PRIVATE snk::input_system PRIVATE snk::window PRIVATE snk::ecs PRIVATE snk::engine_logger)
target_link_libraries(${PROJECT_NAME} PUBLIC snk::desc_system)

#temporary rilation. it will move to "import_component" in the future
target_link_libraries(${PROJECT_NAME} PRIVATE assimp)