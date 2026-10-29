cmake_minimum_required(VERSION 3.16)
project(project_system)
add_library(project_system STATIC)
add_library(snk::project ALIAS project_system)

file(GLOB SE_PROJECT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(project_system PRIVATE ${SE_PROJECT_SOURCES})

snk_component_headers(project_system)

# Must not depend on resource_system: the manifest decides the resource mount points (EPIC-42).
# PUBLIC Boost.JSON: prj_json_text.h and prj_cmake_presets.h take boost::json values.
target_link_libraries(project_system PUBLIC snk::api PRIVATE snk::engine_logger PUBLIC Boost::json PRIVATE $<BUILD_LOCAL_INTERFACE:snk::compiler_warnings>)
# The engine version a project's engine_version is compared with; one source of truth with the SDK package.
target_compile_definitions(project_system PRIVATE SNK_ENGINE_VERSION="${SNK_SDK_VERSION}")
