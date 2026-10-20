cmake_minimum_required(VERSION 3.16)
project(editor_system)
add_library(${PROJECT_NAME} STATIC)
add_library(snk::editor_system ALIAS ${PROJECT_NAME})

file(GLOB_RECURSE SE_EDT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
if(NOT SNK_WITH_ASSIMP)
	list(FILTER SE_EDT_SOURCES EXCLUDE REGEX "/import/edt_(model_importer|filesystem_assimp_io)\\.cpp$")
endif()
target_sources(${PROJECT_NAME} PRIVATE ${SE_EDT_SOURCES})
# Only include/ is public (snk/editor/*); the source root and the subdirs are PRIVATE internals.
snk_component_headers(${PROJECT_NAME})
target_include_directories(${PROJECT_NAME} PRIVATE
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
set(_edt_assimp "")
if(SNK_WITH_ASSIMP)
	snk_usage_only(_edt_assimp assimp)
endif()
if(SNK_WITH_ASSIMP AND SNK_ENGINE_SHARED AND MSVC)
	# A DLL exports nothing it was not told to: assimp and Boost.JSON are linked into the editor
	# as their own static copy. assimp's DefaultLogger is a per-module global, so logging set up in
	# the editor is not seen by the DLL's copy (aiScene does not cross core headers).
	# imgui is stateful and is exported (IMGUI_API).
	set(_edt_assimp assimp Boost::json)
endif()
# snk::mcp_server: the agent server of the editor (edt_agent_server, edt_mcp_bridge_host). Nothing under core/ links it.
target_link_libraries(${PROJECT_NAME} PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE ${_edt_assimp} PRIVATE snk::mcp_server)
