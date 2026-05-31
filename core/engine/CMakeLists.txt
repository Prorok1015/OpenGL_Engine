cmake_minimum_required(VERSION 3.16)
project(engine)
add_library(${PROJECT_NAME} STATIC)
add_library(snk::engine ALIAS ${PROJECT_NAME})

file(GLOB SE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_SOURCES})
# PRIVATE: the umbrella provides aggregated object code but no longer re-exports component
# headers. Cross-layer consumers compile against snk::engine_api (EPIC-49 US-49-1/US-49-3).
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::core PRIVATE snk::render PRIVATE snk::input_system PRIVATE snk::scene PRIVATE snk::game PRIVATE snk::gui)
# core_api gives this aggregator's OWN sources (engine_module.cpp etc.) the core headers they
# include, now that snk::core no longer propagates them.
target_link_libraries(${PROJECT_NAME} PRIVATE snk::core_api)

# Public contract of the ENGINE layer (EPIC-49 US-49-3). INTERFACE-only: editor_system and a
# future game_module compile against this, NOT the engine umbrella. Excludes gui_gl_backend
# (concrete backend). Includes snk::core_api so consumers also get the core contract.
# Consumers link snk::engine (object code) AND snk::engine_api (headers).
add_library(engine_api INTERFACE)
add_library(snk::engine_api ALIAS engine_api)
target_link_libraries(engine_api INTERFACE
	snk::core_api
	snk::scene
	snk::render
	snk::gui
	snk::input_system
	snk::game
)

add_subdirectory("input")
add_subdirectory("gui/backends/interface")
add_subdirectory("gui/backends/opengl")
add_subdirectory("gui/gui_system")
add_subdirectory("render/render_system")
add_subdirectory("game_system")
add_subdirectory("scene")
add_subdirectory("image")