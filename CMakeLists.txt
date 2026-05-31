cmake_minimum_required(VERSION 3.16)
project(build_all)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ENGINE_PROFILE_MODE "NONE" CACHE STRING "Profiling mode: NONE, INTERNAL, TRACY")
set_property(CACHE ENGINE_PROFILE_MODE PROPERTY STRINGS NONE INTERNAL TRACY)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-gdwarf -fstandalone-debug)
    string(REPLACE "-gcodeview" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "-gcodeview" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

set(THIRD_PARTY_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib3dparty")
set(ENGINE_CORE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/core")
set(SE_PROJECT_SOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")

# Centralized third-party configuration (cache vars, slim Boost) — must run before
# any add_subdirectory so every submodule sees consistent options (EPIC-41 US-41-1).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ThirdParty)

# Engine layers (core/core + core/engine). Brought in here so the Editor and the
# unit_tests executable consume the already-configured targets instead of having
# editor_system re-configure core via add_subdirectory (EPIC-49 US-49-2).
add_subdirectory(core)

add_subdirectory(Editor)

enable_testing()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "unittests/*.cpp")
list(REMOVE_ITEM TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/unittests/unit_tests.cpp")

add_executable(unit_tests unittests/unit_tests.cpp ${TEST_SOURCES})

target_link_libraries(unit_tests PRIVATE snk::editor_system PRIVATE snk::engine PRIVATE snk::config_system PRIVATE Boost::unit_test_framework)

# Profiler always enabled for tests (so profiler tests can run)
target_compile_definitions(unit_tests PRIVATE ENGINE_PROFILE_INTERNAL=1)
if(ENGINE_PROFILE_MODE STREQUAL "NONE")
  target_sources(unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/core/core/common/eng_profiler.cpp")
endif()

add_test(NAME unit_tests COMMAND unit_tests)
