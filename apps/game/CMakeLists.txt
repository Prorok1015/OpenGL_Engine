cmake_minimum_required(VERSION 3.16)
project(game)

# The shipping host (#591, docs/roadmap/design/DISTRIBUTION.md): the static engine and the project's game modules
# compiled in, nothing from Editor/. The shared configurations build the editor instead.
if(SNK_ENGINE_SHARED)
	message(STATUS "Shared engine (SNK_ENGINE_SHARED=ON): the game host is not built (#591)")
	return()
endif()

add_executable(${PROJECT_NAME} main.cpp game_host_module.cpp game_host_module.h)
target_link_libraries(${PROJECT_NAME} PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE snk::sanitizers)
