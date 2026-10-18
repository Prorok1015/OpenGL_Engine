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
# link it. Experimental, Linux only for now (EPIC-29 #59).
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
if(MSVC)
    add_compile_options(/d1trimfile:${CMAKE_CURRENT_SOURCE_DIR}/)
else()
    add_compile_options(-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/=)
endif()

# Centralized third-party configuration (cache vars, slim Boost) — must run before
# any add_subdirectory so every submodule sees consistent options (EPIC-41 US-41-1).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ThirdParty)
# FetchContent'd Boost lives outside the source tree: remap it too, plus the symlink-resolved
# source root (e.g. submodules linked from another checkout).
if(MSVC)
    if(boost_SOURCE_DIR)
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

add_executable(unit_tests tests/unit_tests.cpp ${TEST_SOURCES})

target_link_libraries(unit_tests PRIVATE snk::editor_system PRIVATE snk::engine PRIVATE snk::engine_api PRIVATE Boost::unit_test_framework PRIVATE snk::sanitizers)

# Profiler always enabled for tests (so profiler tests can run)
target_compile_definitions(unit_tests PRIVATE ENGINE_PROFILE_INTERNAL=1)
# desc_repo_roundtrip_tests loads the descs the repository ships.
target_compile_definitions(unit_tests PRIVATE SNK_EDITOR_RES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Editor/res")
target_compile_definitions(unit_tests PRIVATE SNK_CORE_RES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/core/res")

add_test(NAME unit_tests COMMAND unit_tests)

snk_apply_target_defaults()
