cmake_minimum_required(VERSION 3.16)
project(engine)
if(SNK_ENGINE_SHARED)
	# The umbrella is the shared library itself; see the WHOLE_ARCHIVE block at the end.
	set(SNK_ENGINE_TARGET snk_engine)
	add_library(snk_engine SHARED)
	# Consumers (incl. the installed SDK, whose package cannot see the root directory's
	# add_compile_definitions) need it for the SNK_API import side.
	target_compile_definitions(snk_engine INTERFACE SNK_ENGINE_SHARED)
else()
	set(SNK_ENGINE_TARGET ${PROJECT_NAME})
	add_library(${PROJECT_NAME} STATIC)
endif()
add_library(snk::engine ALIAS ${SNK_ENGINE_TARGET})

file(GLOB SE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${SNK_ENGINE_TARGET} PRIVATE ${SE_SOURCES})
# PRIVATE: the umbrella provides aggregated object code but no longer re-exports component
# headers. Cross-layer consumers compile against snk::engine_api (EPIC-49 US-49-1/US-49-3).
target_include_directories(${SNK_ENGINE_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${SNK_ENGINE_TARGET} PRIVATE snk::core PRIVATE snk::render PRIVATE snk::input_system PRIVATE snk::scene PRIVATE snk::game PRIVATE snk::gui)
# core_api gives this aggregator's OWN sources (engine_module.cpp etc.) the core headers they
# include, now that snk::core no longer propagates them.
target_link_libraries(${SNK_ENGINE_TARGET} PRIVATE snk::core_api)

# Public contract of the ENGINE layer (EPIC-49 US-49-3). INTERFACE-only: editor_system and a
# future game_module compile against this, NOT the engine umbrella. Excludes gui_gl_backend
# (concrete backend). Includes snk::core_api so consumers also get the core contract.
# Consumers link snk::engine (object code) AND snk::engine_api (headers).
add_library(engine_api INTERFACE)
add_library(snk::engine_api ALIAS engine_api)
snk_usage_only(_engine_api_libs
	snk::core_api
	snk::scene
	snk::render
	snk::gui
	snk::input_system
	snk::game
)
target_link_libraries(engine_api INTERFACE ${_engine_api_libs})

add_subdirectory("input")
add_subdirectory("gui/backends/interface")
add_subdirectory("gui/backends/opengl")
add_subdirectory("gui/gui_system")
add_subdirectory("render/render_system")
add_subdirectory("game_system")
add_subdirectory("scene")
add_subdirectory("image")

# Shared variant: snk_engine carries ALL engine and core component code. Archives are linked
# whole so nothing is dropped just because the library itself references no symbol from it (the
# editor and the tests do). imgui and assimp are included because the editor calls them directly
# and must not link a second copy of their globals.
# $<LINK_LIBRARY:WHOLE_ARCHIVE,x> cannot be used: the components also reach each other (and
# snk_engine) through their PRIVATE links as plain items, and CMake rejects an item that is both
# whole-archive and default. The linker options below act on the archive files directly; the
# plain occurrences on the link line are then redundant but harmless.
if(SNK_ENGINE_SHARED)
	set(_snk_whole_archive
		snk::core snk::common snk::engine_logger snk::config_system snk::application snk::window
		snk::resource_system snk::desc_system snk::ecs snk::drv_opengl snk::input_system snk::render
		snk::game snk::scene snk::gui snk::gui_gl_backend snk::image imgui assimp)
	if(MSVC)
		foreach(_lib IN LISTS _snk_whole_archive)
			target_link_options(snk_engine PRIVATE "LINKER:/WHOLEARCHIVE:$<TARGET_FILE:${_lib}>")
		endforeach()
	else()
		set(_snk_wa_files "")
		foreach(_lib IN LISTS _snk_whole_archive)
			list(APPEND _snk_wa_files "$<TARGET_FILE:${_lib}>")
		endforeach()
		target_link_options(snk_engine PRIVATE "LINKER:--whole-archive" ${_snk_wa_files} "LINKER:--no-whole-archive")
	endif()
endif()
