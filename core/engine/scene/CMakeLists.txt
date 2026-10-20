cmake_minimum_required(VERSION 3.16)
project(scene)
add_library(${PROJECT_NAME} STATIC)
add_library(snk::scene ALIAS ${PROJECT_NAME})

file(GLOB_RECURSE SE_SCENE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_SCENE_SOURCES})
snk_component_headers(${PROJECT_NAME})

# scene does NOT use gui (verified: zero gui includes / gui:: usage) — the link was dead and is a
# layer violation (scene must not depend on gui). Removed (EPIC-49 US-49-9). gui takes the rendered
# scene texture from the renderer via the existing extractor/frame_context seam, not the reverse.
# PUBLIC render: scn_material_desc.h names the material description that lives in render (#236).
target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PUBLIC snk::render PRIVATE snk::resource_system PRIVATE snk::input_system PRIVATE snk::window PRIVATE snk::ecs PRIVATE snk::engine_logger)
target_link_libraries(${PROJECT_NAME} PUBLIC snk::desc_system)

#temporary rilation. it will move to "import_component" in the future
target_link_libraries(${PROJECT_NAME} PRIVATE assimp)