cmake_minimum_required(VERSION 3.27)

# Before anything else, the compiler check included: the SDK tells generated headers from source headers by the
# build tree (cmake/SnkInstall.cmake, _snk_sdk_include_dest), which breaks when the build tree holds the sources (#582).
file(REAL_PATH "${CMAKE_SOURCE_DIR}" _snk_source_dir)
file(REAL_PATH "${CMAKE_BINARY_DIR}" _snk_binary_dir)
# A trailing "/" (a filesystem root: / or C:/) is dropped before one is appended; Windows paths ignore case.
string(REGEX REPLACE "/+$" "" _snk_source_dir "${_snk_source_dir}")
string(REGEX REPLACE "/+$" "" _snk_binary_dir "${_snk_binary_dir}")
if(CMAKE_HOST_WIN32)
    string(TOLOWER "${_snk_source_dir}" _snk_source_dir)
    string(TOLOWER "${_snk_binary_dir}" _snk_binary_dir)
endif()
string(FIND "${_snk_source_dir}/" "${_snk_binary_dir}/" _snk_source_in_binary)
if(_snk_source_in_binary EQUAL 0)
    message(FATAL_ERROR "snake: in-source builds are not supported (#582): the build directory ${_snk_binary_dir} "
        "contains the sources. Configure into a subdirectory, e.g. cmake --preset dev-debug or cmake -B build/dev. "
        "Remove CMakeCache.txt and CMakeFiles/ that this attempt left in ${_snk_binary_dir}.")
endif()
unset(_snk_source_dir)
unset(_snk_binary_dir)
unset(_snk_source_in_binary)

# SNK_COMPILER_CACHE / SNK_PCH. Before project(): the MSVC debug-info policy it sets must precede
# it, and NO_POLICY_SCOPE keeps that policy in this scope.
include(cmake/SnkBuildCache.cmake NO_POLICY_SCOPE)

project(build_all)

# The oldest MSVC the engine supports (owner decision, #219): VS 2022 17.14, toolset 14.44. The SDK's config refuses a game
# module older than the MSVC that built the engine, so the engine is built with the oldest one CI pins.
if(MSVC AND MSVC_VERSION LESS 1944)
    message(FATAL_ERROR "snake: MSVC ${MSVC_VERSION} is older than the oldest supported 1944 (Visual Studio 2022 17.14, "
        "toolset 14.44). Update Visual Studio, or pick a newer toolset (-T version=14.44 or the vcvars -vcvars_ver).")
endif()

# /MD everywhere, Shipping included (owner decision R11, #223): the editor, snk_engine.dll and game.dll share
# one C++ runtime, which the release package bundles. Debug builds use its debug variant (/MDd), never shipped.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
if(MSVC)
    # The PDBs of the Release package (#223): /DEBUG, with the optimizations it would otherwise turn off. /FS: without
    # a compiler cache, the Release /Zi of a shared-engine build (cmake/SnkBuildCache.cmake) writes compiler PDBs from
    # parallel compiles.
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    if(SNK_ENGINE_SHARED)
        add_compile_options($<$<CONFIG:Release>:/FS>)
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# PIC everywhere (third-party included: assimp, glfw, ... end up inside a shared engine library
# when SNK_ENGINE_SHARED is on). Must be set before any target is created.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# One shared library (snk_engine) instead of static engine components. The editor and game modules
# (game.dll) always use it (owner decision R11, #223); the static engine is for Shipping (#23) and one
# CI build that keeps that path compiling. A static build has no editor executable.
option(SNK_ENGINE_SHARED "Build the engine layers as one shared library (snk_engine)" ON)
if(SNK_ENGINE_SHARED)
    # $<COMPILE_ONLY:> (usage requirements without linking) keeps consumers from linking the
    # component archives a second time.
    if(CMAKE_VERSION VERSION_LESS 3.27)
        message(FATAL_ERROR "SNK_ENGINE_SHARED requires CMake 3.27+ (found ${CMAKE_VERSION})")
    endif()
    add_compile_definitions(SNK_ENGINE_SHARED)
endif()

# Debug libraries carry a "d" (snk_engined.dll) so a Debug and a Release install can share one SDK
# prefix (#223). Before any target: it only initializes DEBUG_POSTFIX of targets created after it.
if(NOT DEFINED CMAKE_DEBUG_POSTFIX)
    set(CMAKE_DEBUG_POSTFIX d)
endif()

set(ENGINE_PROFILE_MODE "NONE" CACHE STRING "Profiling mode: NONE, INTERNAL, TRACY")
set_property(CACHE ENGINE_PROFILE_MODE PROPERTY STRINGS NONE INTERNAL TRACY)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-gdwarf -fstandalone-debug)
    string(REPLACE "-gcodeview" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "-gcodeview" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

set(THIRD_PARTY_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib3dparty")
set(ENGINE_CORE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/core")

# Keep absolute source paths out of the binaries: __FILE__ (asserts, GL error checks, logging
# in project and third-party code) becomes relative to the source root. Only the __FILE__ macro
# is remapped, not debug info, so debuggers and gcov/coverage still see the real paths.
# sccache takes the path in /d1trimfile:<dir> for a second input file and refuses to cache every
# compile ("multiple input files"), so with sccache MSVC keeps absolute __FILE__ paths.
set(_snk_msvc_trimfile OFF)
if(MSVC AND NOT SNK_COMPILER_CACHE_TOOL STREQUAL "sccache")
    set(_snk_msvc_trimfile ON)
endif()
if(MSVC)
    if(_snk_msvc_trimfile)
        add_compile_options(/d1trimfile:${CMAKE_CURRENT_SOURCE_DIR}/)
    endif()
else()
    add_compile_options(-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/=)
endif()

# Centralized third-party configuration (cache vars, slim Boost) — must run before
# any add_subdirectory so every submodule sees consistent options (EPIC-41 US-41-1).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ThirdParty)
# 1/0 for `#if SNK_HAS_ASSIMP` in the editor, tools and tests; every project target sees it.
if(SNK_WITH_ASSIMP)
    add_compile_definitions(SNK_HAS_ASSIMP=1)
else()
    add_compile_definitions(SNK_HAS_ASSIMP=0)
endif()
# FetchContent'd Boost lives outside the source tree: remap it too, plus the symlink-resolved
# source root (e.g. submodules linked from another checkout).
if(MSVC)
    if(boost_SOURCE_DIR AND _snk_msvc_trimfile)
        add_compile_options(/d1trimfile:${boost_SOURCE_DIR}/)
    endif()
else()
    get_filename_component(_snk_real_src "${CMAKE_CURRENT_SOURCE_DIR}/lib3dparty" REALPATH)
    add_compile_options(-fmacro-prefix-map=${_snk_real_src}/=lib3dparty/)
    if(boost_SOURCE_DIR)
        add_compile_options(-fmacro-prefix-map=${boost_SOURCE_DIR}/=boost/)
    endif()
endif()
# SNK_SANITIZE option + snk::sanitizers interface target (project targets only).
include(Sanitizers)
# snk::compiler_warnings interface target (project targets only, via snk::common).
include(CompilerWarnings)
# snk_target_defaults(): hidden visibility for project targets (applied at the end of this file).
include(SnkTarget)
# snk_pch_*(): shared precompiled header (SNK_PCH), applied by snk_apply_target_defaults().
include(SnkPch)
# snk_install_sdk(): install/export rules and the snake_sdk package (called at the end).
include(SnkInstall)
# snk_check_mcp_isolation(): the MCP server module stays out of the engine, games and the SDK.
include(SnkMcpIsolation)
# snk_check_engine_copies(): a binary that links the shared engine links none of its components again (#874).
include(SnkEngineCopies)
# snk_add_game_host(): a shipping executable with its game module compiled in (#592).
include(SnkGameHost)

# Engine layers (core/core + core/engine). Brought in here so the Editor and the
# unit_tests executable consume the already-configured targets instead of having
# editor_system re-configure core via add_subdirectory (EPIC-49 US-49-2).
add_subdirectory(core)
# EnTT ids cross binaries (game modules, snk_engine) only in the default mode: ENTT_STANDARD_CPP turns
# entt::type_hash into a per-binary counter and ENTT_ID_TYPE changes the ABI (#218). A definition set on a
# single target is caught by snk/ecs/ecs_entt_abi.h, which every ECS header with EnTT includes.
set(_snk_entt_flags "${CMAKE_CXX_FLAGS}")
foreach(_snk_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER "${_snk_config}" _snk_config)
    string(APPEND _snk_entt_flags " ${CMAKE_CXX_FLAGS_${_snk_config}}")
endforeach()
get_directory_property(_snk_dir_defs COMPILE_DEFINITIONS)
get_target_property(_snk_entt_defs EnTT INTERFACE_COMPILE_DEFINITIONS)
if("${_snk_entt_flags} ${_snk_dir_defs} ${_snk_entt_defs}" MATCHES "ENTT_STANDARD_CPP|ENTT_ID_TYPE")
    message(FATAL_ERROR "ENTT_STANDARD_CPP / ENTT_ID_TYPE are not supported (docs/roadmap/design/SPIKE-218-entt-dll.md)")
endif()

add_subdirectory(Editor)
# The shipping host `game` (#591): static engine only, the counterpart of the editor's shared-only executable.
add_subdirectory(apps/game)
add_subdirectory(tools/project_migration)
add_subdirectory(tools/desc_resave)
add_subdirectory(tools/model_decoder)
snk_isolate_third_party_warnings()

enable_testing()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "tests/*.cpp")
list(REMOVE_ITEM TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit_tests.cpp")
if(NOT SNK_WITH_ASSIMP)
    # These drive the assimp importers, or load the shipped levels that reference models.
    list(FILTER TEST_SOURCES EXCLUDE REGEX "/tests/(scn_model_importer_tests|edt_import_cancel_tests|edt_import_export_format_tests|edt_agent_model_scan_tests|desc_repo_roundtrip_tests)\\.cpp$")
endif()

add_executable(unit_tests tests/unit_tests.cpp ${TEST_SOURCES})

target_link_libraries(unit_tests PRIVATE snk::editor_system PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE Boost::unit_test_framework PRIVATE snk::sanitizers)
target_link_libraries(unit_tests PRIVATE snk::project_migration)
# The MCP server module is tested on its own (tests/mcp_*.cpp): a fake host over sockets and in process. The socket tests use Asio directly.
target_link_libraries(unit_tests PRIVATE snk::mcp_server PRIVATE Boost::asio)
# The tests create the null and validation backends: their public headers (snk/driver_null, snk/driver_validation).
snk_usage_only(_snk_test_drivers snk::drv_null snk::drv_validation)
target_link_libraries(unit_tests PRIVATE ${_snk_test_drivers})

# rnd_null_driver_tests and rnd_validation_tests drive the internal null and validation backends; both are
# linked through render (shared: snk_engine). rnd_per_draw_data_tests runs render's internal frame_upload_pass,
# rnd_shader_load_tests its internal ShaderManager.
target_include_directories(unit_tests PRIVATE "$<TARGET_PROPERTY:drv_null,SOURCE_DIR>" "$<TARGET_PROPERTY:drv_validation,SOURCE_DIR>" "$<TARGET_PROPERTY:render,SOURCE_DIR>" "$<TARGET_PROPERTY:render,SOURCE_DIR>/passes")

# White-box tests: PRIVATE access to the internal (non-public) editor_system headers, test-only.
target_include_directories(unit_tests PRIVATE "$<TARGET_PROPERTY:editor_system,SOURCE_DIR>" "$<TARGET_PROPERTY:editor_system,SOURCE_DIR>/assets" "$<TARGET_PROPERTY:editor_system,SOURCE_DIR>/core" "$<TARGET_PROPERTY:editor_system,SOURCE_DIR>/controller" "$<TARGET_PROPERTY:editor_system,SOURCE_DIR>/panels" "$<TARGET_PROPERTY:editor_system,SOURCE_DIR>/edt_component_renderers" "$<TARGET_PROPERTY:editor_system,SOURCE_DIR>/import" "$<TARGET_PROPERTY:editor_system,SOURCE_DIR>/gizmo" "$<TARGET_PROPERTY:editor_system,SOURCE_DIR>/input" "$<TARGET_PROPERTY:editor_system,SOURCE_DIR>/game_build" "$<TARGET_PROPERTY:editor_system,SOURCE_DIR>/vcs" "$<TARGET_PROPERTY:editor_system,SOURCE_DIR>/forge")

# Profiler always enabled for tests (so profiler tests can run)
target_compile_definitions(unit_tests PRIVATE ENGINE_PROFILE_INTERNAL=1)
if(MSVC)
	# Test files that describe and register several component schemas exceed the default section count (C1128).
	target_compile_options(unit_tests PRIVATE /bigobj)
endif()
# desc_repo_roundtrip_tests loads the descs the repository ships.
if(SNK_WITH_ASSIMP AND NOT TARGET snk_model_decoder)
	message(FATAL_ERROR "snk_model_decoder is not configured: the decoder process tests would silently not run (#647)")
endif()
if(TARGET snk_model_decoder)
	# The process tests (edt_decode_runner_tests) start the real decoder, and the test build of it for the self-test flags.
	add_dependencies(unit_tests snk_model_decoder snk_model_decoder_selftest)
	target_compile_definitions(unit_tests PRIVATE SNK_MODEL_DECODER_PATH="$<TARGET_FILE:snk_model_decoder>"
		SNK_MODEL_DECODER_SELFTEST_PATH="$<TARGET_FILE:snk_model_decoder_selftest>")
endif()
target_compile_definitions(unit_tests PRIVATE SNK_TEST_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/projects/sandbox")
target_compile_definitions(unit_tests PRIVATE SNK_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/projects")
target_compile_definitions(unit_tests PRIVATE SNK_EDITOR_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Editor/data")
target_compile_definitions(unit_tests PRIVATE SNK_CORE_RES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/core/res")
# prj::current_engine_version() must report exactly this (tests/prj_version_tests.cpp).
target_compile_definitions(unit_tests PRIVATE SNK_TEST_SDK_VERSION="${SNK_SDK_VERSION}")
# rnd_render_context_tests reads the sources of the render passes to check what they include.
target_compile_definitions(unit_tests PRIVATE SNK_RENDER_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/core/engine/render/render_system")

if(WIN32 AND SNK_ENGINE_SHARED)
	# Windows has no rpath: snk_engine.dll must sit next to the executable.
	add_custom_command(TARGET unit_tests POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:unit_tests> $<TARGET_FILE_DIR:unit_tests>
		COMMAND_EXPAND_LISTS)
endif()

add_test(NAME unit_tests COMMAND unit_tests)

if(SNK_ENGINE_SHARED)
    # A game module loaded from disk by unit_tests (tests/module_dll).
    add_subdirectory(tests/module_dll)
    # A module that registers into the editor through snk_editor_api (#587).
    add_subdirectory(tests/editor_api_dll)
else()
    # A game module compiled into a static host (snk_add_game_host, #592), run headless by CI.
    add_subdirectory(tests/static_game)
endif()

snk_pch_create_owner()
snk_apply_target_defaults()
# Fails the configure if the engine, a game target or the SDK can reach snk_mcp_server (tools/ci/check_mcp_isolation.sh is the source-level twin).
snk_check_mcp_isolation()
snk_check_engine_copies()
snk_install_sdk()

# Assimp hard-codes /Zi into its Debug C++ flags (lib3dparty/assimp/CMakeLists.txt); sccache then
# expects a .pdb that the embedded-debug-info (/Z7) build never writes and fails the compile.
# Assimp alone builds uncached in that case.
if(MSVC AND SNK_COMPILER_CACHE_TOOL STREQUAL "sccache" AND TARGET assimp
		AND CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
	set_target_properties(assimp PROPERTIES CXX_COMPILER_LAUNCHER "")
endif()
