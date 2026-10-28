# The game module of this project: a library the editor loads (DISTRIBUTION.md, model 2). The editor builds it with
# Build > Build Game; by hand, from this folder:
#   cmake --preset debuggame && cmake --build --preset debuggame
# with SNAKE_SDK_DIR (or snake_sdk_DIR) set to the folder holding snake_sdkConfig.cmake: <editor>/lib/cmake/snake_sdk
# in an installed editor, <build>/snake_sdk in an engine build tree.
cmake_minimum_required(VERSION 3.24)
project(game LANGUAGES C CXX)

# The module's name in ../project.snkproj: the editor loads ../build/bin/<name>.dll (lib<name>.so).
set(SNK_GAME_MODULE game)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# One C runtime with the engine in every configuration (/MD): STL types cross the library boundary.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)

# DebugGame: the game's code unoptimized and with symbols, against the engine's release runtime (no _DEBUG). With
# GCC/Clang NDEBUG stays undefined, which the host compares too: such a module loads into a Debug engine build.
set(CMAKE_CONFIGURATION_TYPES DebugGame Release)
if(MSVC)
	set(CMAKE_C_FLAGS_DEBUGGAME "/Od /Ob0 /Zi")
	set(CMAKE_CXX_FLAGS_DEBUGGAME "/Od /Ob0 /Zi")
	set(CMAKE_SHARED_LINKER_FLAGS_DEBUGGAME "/DEBUG /INCREMENTAL:NO")
else()
	set(CMAKE_C_FLAGS_DEBUGGAME "-O0 -g")
	set(CMAKE_CXX_FLAGS_DEBUGGAME "-O0 -g")
	set(CMAKE_SHARED_LINKER_FLAGS_DEBUGGAME "")
endif()

if(NOT snake_sdk_DIR AND DEFINED ENV{SNAKE_SDK_DIR})
	set(snake_sdk_DIR "$ENV{SNAKE_SDK_DIR}" CACHE PATH "Folder holding snake_sdkConfig.cmake")
endif()
find_package(snake_sdk 0.2 CONFIG REQUIRED)
if(NOT snake_sdk_ENGINE_SHARED)
	message(FATAL_ERROR "The SDK in ${snake_sdk_DIR} has a static engine: a game module needs the shared engine "
		"(SNK_ENGINE_SHARED), or it would carry its own copy of it.")
endif()

add_library(game SHARED src/game_module.cpp)
target_link_libraries(game PRIVATE snk::engine snk::engine_api)
target_compile_definitions(game PRIVATE "SNK_GAME_MODULE_NAME=\"${SNK_GAME_MODULE}\"")

# Always ../build/bin/<name>, whoever builds it and in any configuration: no "d" postfix (an engine build tree may set
# CMAKE_DEBUG_POSTFIX) and no per-configuration subfolder ($<1:...> keeps multi-config generators from adding one).
set(_game_bin "${CMAKE_CURRENT_SOURCE_DIR}/../build/bin")
set_target_properties(game PROPERTIES
	OUTPUT_NAME "${SNK_GAME_MODULE}"
	DEBUG_POSTFIX ""
	RUNTIME_OUTPUT_DIRECTORY "$<1:${_game_bin}>"
	LIBRARY_OUTPUT_DIRECTORY "$<1:${_game_bin}>"
	PDB_OUTPUT_DIRECTORY "$<1:${_game_bin}>")
