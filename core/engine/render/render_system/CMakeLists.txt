cmake_minimum_required(VERSION 3.16)
project(render)
add_library(render STATIC)
add_library(snk::render ALIAS render)

file(GLOB_RECURSE SE_RENDER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_RENDER_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# rnd_material_compiler consumes the POD scn::material_desc header (EPIC-49 US-49-5). material_desc
# is header-only data (its methods live in scene's .cpp and the compiler calls none of them), so this
# is an include-dir dependency only — render does NOT link scene, avoiding a link cycle (scene links
# render). Header reachable for the compiler .cpp and any consumer of rnd_material_compiler.h.
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../scene)


target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::window PRIVATE snk::resource_system PRIVATE snk::desc_system PRIVATE snk::input_system PUBLIC snk::drv_interface PRIVATE snk::engine_logger)