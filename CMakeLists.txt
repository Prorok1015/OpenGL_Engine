cmake_minimum_required(VERSION 3.24)

# SNK_COMPILER_CACHE / SNK_PCH. Before project(): the MSVC debug-info policy it sets must precede
# it, and NO_POLICY_SCOPE keeps that policy in this scope.
include(cmake/SnkBuildCache.cmake NO_POLICY_SCOPE)

project(build_all)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# PIC everywhere (third-party included: assimp, glfw, ... end up inside a shared engine library
# when SNK_ENGINE_SHARED is on). Must be set before any target is created.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# One shared library (snk_engine) instead of static engine components; the editor and the tests
# link it. Experimental; CI covers Linux GCC and Windows MSVC (EPIC-29 #59, #198).
option(SNK_ENGINE_SHARED "Build the engine layers as one shared library (snk_engine)" OFF)
if(SNK_ENGINE_SHARED)
    # $<COMPILE_ONLY:> (usage requirements without linking) keeps consumers from linking the
    # component archives a second time.
    if(CMAKE_VERSION VERSION_LESS 3.27)
        message(FATAL_ERROR "SNK_ENGINE_SHARED requires CMake 3.27+ (found ${CMAKE_VERSION})")
    endif()
    add_compile_definitions(SNK_ENGINE_SHARED)
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

# Engine layers (core/core + core/engine). Brought in here so the Editor and the
# unit_tests executable consume the already-configured targets instead of having
# editor_system re-configure core via add_subdirectory (EPIC-49 US-49-2).
add_subdirectory(core)

add_subdirectory(Editor)
add_subdirectory(tools/desc_resave)
snk_isolate_third_party_warnings()

enable_testing()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "tests/*.cpp")
list(REMOVE_ITEM TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit_tests.cpp")
if(NOT SNK_WITH_ASSIMP)
    # These drive the assimp importers, or load the shipped levels that reference models.
    list(FILTER TEST_SOURCES EXCLUDE REGEX "/tests/(scn_model_importer_tests|edt_import_cancel_tests|desc_repo_roundtrip_tests)\\.cpp$")
endif()

add_executable(unit_tests tests/unit_tests.cpp ${TEST_SOURCES})

target_link_libraries(unit_tests PRIVATE snk::editor_system PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE Boost::unit_test_framework PRIVATE snk::sanitizers)
# The MCP server module is tested on its own (tests/mcp_*.cpp): a fake host over sockets and in process. The socket tests use Asio directly.
target_link_libraries(unit_tests PRIVATE snk::mcp_server PRIVATE Boost::asio)
# The tests create the null and validation backends: their public headers (snk/driver_null, snk/driver_validation).
snk_usage_only(_snk_test_drivers snk::drv_null snk::drv_validation)
target_link_libraries(unit_tests PRIVATE ${_snk_test_drivers})

# rnd_null_driver_tests and rnd_validation_tests drive the internal null and validation backends; both are
# linked through render (shared: snk_engine). rnd_per_draw_data_tests runs render's internal frame_upload_pass,
# rnd_shader_load_tests its internal ShaderManager.
target_include_directories(unit_tests PRIVATE "$<TARGET_PROPERTY:drv_null,SOURCE_DIR>" "$<TARGET_PROPERTY:drv_validation,SOURCE_DIR>" "$<TARGET_PROPERTY:render,SOURCE_DIR>" "$<TARGET_PROPERTY:render,SOURCE_DIR>/passes")

# Profiler always enabled for tests (so profiler tests can run)
target_compile_definitions(unit_tests PRIVATE ENGINE_PROFILE_INTERNAL=1)
# desc_repo_roundtrip_tests loads the descs the repository ships.
target_compile_definitions(unit_tests PRIVATE SNK_EDITOR_RES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Editor/res")
target_compile_definitions(unit_tests PRIVATE SNK_CORE_RES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/core/res")
# rnd_render_context_tests reads the sources of the render passes to check what they include.
target_compile_definitions(unit_tests PRIVATE SNK_RENDER_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/core/engine/render/render_system")

if(WIN32 AND SNK_ENGINE_SHARED)
	# Windows has no rpath: snk_engine.dll must sit next to the executable.
	add_custom_command(TARGET unit_tests POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:unit_tests> $<TARGET_FILE_DIR:unit_tests>
		COMMAND_EXPAND_LISTS)
endif()

add_test(NAME unit_tests COMMAND unit_tests)

snk_pch_create_owner()
snk_apply_target_defaults()
# Fails the configure if the engine, a game target or the SDK can reach snk_mcp_server (tools/ci/check_mcp_isolation.sh is the source-level twin).
snk_check_mcp_isolation()
snk_install_sdk()

# Assimp hard-codes /Zi into its Debug C++ flags (lib3dparty/assimp/CMakeLists.txt); sccache then
# expects a .pdb that the embedded-debug-info (/Z7) build never writes and fails the compile.
# Assimp alone builds uncached in that case.
if(MSVC AND SNK_COMPILER_CACHE_TOOL STREQUAL "sccache" AND TARGET assimp
		AND CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
	set_target_properties(assimp PROPERTIES CXX_COMPILER_LAUNCHER "")
endif()
