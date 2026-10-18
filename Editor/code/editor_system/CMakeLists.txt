cmake_minimum_required(VERSION 3.16)
project(editor_system)
add_library(${PROJECT_NAME} STATIC)
add_library(snk::editor_system ALIAS ${PROJECT_NAME})

file(GLOB_RECURSE SE_EDT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_EDT_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/core"
	"${CMAKE_CURRENT_SOURCE_DIR}/controller"
	"${CMAKE_CURRENT_SOURCE_DIR}/panels"
	"${CMAKE_CURRENT_SOURCE_DIR}/edt_component_renderers"
	"${CMAKE_CURRENT_SOURCE_DIR}/import"
	"${CMAKE_CURRENT_SOURCE_DIR}/gizmo"
	"${CMAKE_CURRENT_SOURCE_DIR}/input"
)

# snk::engine = aggregated object code (symbols); snk::engine_api = the public header surface
# (EPIC-49 US-49-1/US-49-3). The umbrella no longer leaks all component headers.
# assimp: the editor calls it directly; in the shared build it lives in snk_engine.
snk_usage_only(_edt_assimp assimp)
target_link_libraries(${PROJECT_NAME} PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE ${_edt_assimp})
