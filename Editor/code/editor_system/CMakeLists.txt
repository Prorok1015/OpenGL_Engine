cmake_minimum_required(VERSION 3.16)
project(editor_system)
add_library(${PROJECT_NAME} STATIC)
add_library(snk::editor_system ALIAS ${PROJECT_NAME})

# snk_editor_api: the part of the editor a game_editor module calls into (the registries, editor_context and the
# services it hands out, panel_base; #587). A shared library next to snk_engine when the engine is shared, so the
# editor and a module work on the same registries; a static one (not shipped, tests only) otherwise. It holds no
# scene, prefab or MCP code: editor_system hands those in (document_manager's preview_factory, editor_context::adopt).
if(SNK_ENGINE_SHARED)
	set(_edt_api_type SHARED)
else()
	set(_edt_api_type STATIC)
endif()
add_library(snk_editor_api ${_edt_api_type})
add_library(snk::editor_api ALIAS snk_editor_api)

file(GLOB_RECURSE SE_EDT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
if(NOT SNK_WITH_ASSIMP)
	list(FILTER SE_EDT_SOURCES EXCLUDE REGEX "/import/edt_(model_importer|filesystem_assimp_io)\\.cpp$")
endif()
set(SE_EDT_API_SOURCES
	core/edt_action_registry.cpp core/edt_asset_kind_registry.cpp core/edt_dialog_service.cpp core/edt_document_history.cpp
	core/edt_document_manager.cpp core/edt_editor_context.cpp core/edt_editor_extension.cpp core/edt_editor_services.cpp core/edt_inspector_registry.cpp
	core/edt_job_service.cpp core/edt_panel_registry.cpp core/edt_problem_list.cpp core/edt_registration_scope.cpp core/edt_selection_service.cpp
	core/edt_service_commands.cpp input/edt_file_dialog.cpp panels/edt_panel_base.cpp)
list(TRANSFORM SE_EDT_API_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(REMOVE_ITEM SE_EDT_SOURCES ${SE_EDT_API_SOURCES})
target_sources(${PROJECT_NAME} PRIVATE ${SE_EDT_SOURCES})
target_sources(snk_editor_api PRIVATE ${SE_EDT_API_SOURCES})
# Only include/ is public (snk/editor/*); the source root and the subdirs are PRIVATE internals.
snk_component_headers(${PROJECT_NAME})
snk_component_headers(snk_editor_api)
target_include_directories(snk_editor_api PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/core"
	"${CMAKE_CURRENT_SOURCE_DIR}/input"
	"${CMAKE_CURRENT_SOURCE_DIR}/panels"
)
if(SNK_ENGINE_SHARED)
	# SNK_EDITOR_SHARED reaches every consumer (the editor, the tests, a module of the SDK); SNK_EDITOR_BUILD only the
	# library itself (edt_api.h). The exported std:: members of its classes need /wd4251 /wd4275 on both sides: the
	# library gets them from snk_target_defaults, a consumer through this interface (as snk_engine does).
	target_compile_definitions(snk_editor_api PUBLIC SNK_EDITOR_SHARED PRIVATE SNK_EDITOR_BUILD)
	if(MSVC)
		target_compile_options(snk_editor_api INTERFACE $<$<COMPILE_LANGUAGE:CXX>:/wd4251> $<$<COMPILE_LANGUAGE:CXX>:/wd4275>)
	endif()
endif()
target_include_directories(${PROJECT_NAME} PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/core"
	"${CMAKE_CURRENT_SOURCE_DIR}/controller"
	"${CMAKE_CURRENT_SOURCE_DIR}/panels"
	"${CMAKE_CURRENT_SOURCE_DIR}/edt_component_renderers"
	"${CMAKE_CURRENT_SOURCE_DIR}/import"
	"${CMAKE_CURRENT_SOURCE_DIR}/gizmo"
	"${CMAKE_CURRENT_SOURCE_DIR}/input"
	"${CMAKE_CURRENT_SOURCE_DIR}/game_build"
	"${CMAKE_CURRENT_SOURCE_DIR}/vcs"
	"${CMAKE_CURRENT_SOURCE_DIR}/forge"
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
# Like snk_engine, the library carries its dependencies privately: whoever includes snk/editor links snk::engine and
# snk::engine_api itself (the editor, unit_tests, a module of the SDK; tests/package_consumer does). A public link to
# engine_api would make the SDK export of this real target require engine_api, which the shared set exports as a facade.
target_link_libraries(snk_editor_api PRIVATE snk::engine PRIVATE snk::engine_api)
if(SNK_ENGINE_SHARED AND MSVC)
	# As editor_system: snk_engine.dll exports only SNK_API, Boost.JSON is linked in as its own static copy.
	target_link_libraries(snk_editor_api PRIVATE Boost::json)
endif()
target_link_libraries(${PROJECT_NAME} PUBLIC snk_editor_api PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE ${_edt_assimp} PRIVATE snk::mcp_server)
# forge/curl: the owner-only access list of the request files (Windows security API).
if(WIN32)
	target_link_libraries(${PROJECT_NAME} PRIVATE advapi32)
endif()
# Build > Build Game: the SDK of this build tree (snake_sdkConfig.cmake, exported there) when the editor is not installed.
# A u8 literal: the build path may hold non-ASCII characters, and MSVC without /utf-8 would recode a narrow one.
target_compile_definitions(${PROJECT_NAME} PRIVATE "SNK_BUILD_TREE_SDK_DIR=u8\"${CMAKE_BINARY_DIR}/snake_sdk\"")
