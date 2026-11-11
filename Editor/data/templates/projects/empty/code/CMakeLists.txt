# The game module of this project: a library the editor loads (DISTRIBUTION.md, model 2). The editor builds it with
# Build > Build Game; by hand, from this folder:
#   cmake --preset debuggame && cmake --build --preset debuggame
# with SNAKE_SDK_DIR (or snake_sdk_DIR) set to the folder holding snake_sdkConfig.cmake: <editor>/lib/cmake/snake_sdk
# in an installed editor, <build>/snake_sdk in an engine build tree.
#
# Shipping (SNK_SHIPPING, preset `shipping`): the same code compiled into a game executable with the static engine,
# ../build/shipping/<name>(.exe), instead of a module:
#   cmake --preset shipping && cmake --build --preset shipping
cmake_minimum_required(VERSION 3.24)
project(game LANGUAGES C CXX)

# The module's name in ../project.snkproj: the editor loads ../build/bin/<name>.dll (lib<name>.so).
set(SNK_GAME_MODULE game)
# The editor module's name in ../project.snkproj: ../build/bin/<name>.dll (lib<name>.so).
set(SNK_GAME_EDITOR_MODULE game_editor)
option(SNK_SHIPPING "Build a game executable with the module and the static engine compiled in" OFF)
option(SNK_GAME_EDITOR "Also build the editor module (game_editor), which registers a panel and an inspector renderer into the editor" ON)

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

if(SNK_SHIPPING)
	if(NOT snake_sdk_HAS_STATIC)
		message(FATAL_ERROR "The SDK in ${snake_sdk_DIR} has no static engine (snake_sdk_staticTargets.cmake): "
			"a shipping build compiles the engine into the game executable.")
	endif()
	# The module compiled with SNK_GAME_MODULE_STATIC: an OBJECT library, so its SNK_DECLARE_MODULE is not dropped.
	add_library(game_objects OBJECT src/game_module.cpp)
	snk_add_game_host(game MODULE game_objects NAME ${SNK_GAME_MODULE})
	set_target_properties(game PROPERTIES OUTPUT_NAME "${SNK_GAME_MODULE}")
	return()
endif()

if(NOT snake_sdk_ENGINE_SHARED)
	message(FATAL_ERROR "The SDK in ${snake_sdk_DIR} has a static engine: a game module needs the shared engine "
		"(SNK_ENGINE_SHARED), or it would carry its own copy of it.")
endif()

# Always ../build/bin/<name>, whoever builds it and in any configuration: no "d" postfix (an engine build tree may set
# CMAKE_DEBUG_POSTFIX) and no per-configuration subfolder ($<1:...> keeps multi-config generators from adding one).
# The editor loads a copy of the module from .cache/modules/<pid>-<n>/ with what lies next to it (#435).
function(snk_module_output target name)
	set(_bin "${CMAKE_CURRENT_SOURCE_DIR}/../build/bin")
	set_target_properties(${target} PROPERTIES
		OUTPUT_NAME "${name}"
		DEBUG_POSTFIX ""
		RUNTIME_OUTPUT_DIRECTORY "$<1:${_bin}>"
		LIBRARY_OUTPUT_DIRECTORY "$<1:${_bin}>"
		PDB_OUTPUT_DIRECTORY "$<1:${_bin}>")
	if(MSVC)
		# The module names its PDB by file name only, not by its path in build/bin: the debugger takes the one next to the
		# copy, and the PDB in build/bin stays free for the next build.
		target_link_options(${target} PRIVATE "$<$<CONFIG:DebugGame>:/PDBALTPATH:%_PDB%>")
	else()
		# Libraries next to the module are found next to its copy.
		set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN")
	endif()
endfunction()

add_library(game SHARED src/game_module.cpp)
target_link_libraries(game PRIVATE snk::engine snk::engine_api)
# snk_engine.dll exports only SNK_API, so desc code in the module (copying a prefab_node, boost::json values) needs its
# own copy of Boost.JSON, which the SDK carries for MSVC only (#558). Values the module makes with the default
# storage_ptr hold no pointer into the module (Boost.JSON 1.84: a null storage_ptr reaches the default resource of the
# code that runs, ::operator new/delete), and the module shares the engine's C runtime (/MD), so the engine may keep
# and free them after the module is unloaded or reloaded.
if(MSVC AND TARGET snk::thirdparty_boost_json)
	target_link_libraries(game PRIVATE snk::thirdparty_boost_json)
endif()
target_compile_definitions(game PRIVATE "SNK_GAME_MODULE_NAME=\"${SNK_GAME_MODULE}\"")
snk_module_output(game "${SNK_GAME_MODULE}")

# The module of the editor (kind "editor" in ../project.snkproj, optional): a panel and an inspector renderer. It needs
# the editor API library of the shared SDK. Not part of a shipping build (that returned above).
if(SNK_GAME_EDITOR)
	if(TARGET snk::editor_api)
		add_library(game_editor SHARED src/game_editor.cpp)
		target_link_libraries(game_editor PRIVATE snk::engine snk::engine_api snk::editor_api)
		# Boost.JSON of its own on MSVC, as `game` above (#558).
		if(MSVC AND TARGET snk::thirdparty_boost_json)
			target_link_libraries(game_editor PRIVATE snk::thirdparty_boost_json)
		endif()
		target_compile_definitions(game_editor PRIVATE "SNK_GAME_EDITOR_MODULE_NAME=\"${SNK_GAME_EDITOR_MODULE}\"")
		snk_module_output(game_editor "${SNK_GAME_EDITOR_MODULE}")
	else()
		message(WARNING "The SDK in ${snake_sdk_DIR} has no editor API library (snk::editor_api): game_editor is not built.")
	endif()
endif()
