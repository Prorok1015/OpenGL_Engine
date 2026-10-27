cmake_minimum_required(VERSION 3.16)
project(project_system)
add_library(project_system STATIC)
add_library(snk::project ALIAS project_system)

file(GLOB SE_PROJECT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(project_system PRIVATE ${SE_PROJECT_SOURCES})

snk_component_headers(project_system)

# Must not depend on resource_system: the manifest decides the resource mount points (EPIC-42).
# PUBLIC application: prj_modules.h loads the manifest's modules through app::modules::module_loader.
target_link_libraries(project_system PUBLIC snk::api PUBLIC snk::application PRIVATE snk::engine_logger PRIVATE Boost::json PRIVATE $<BUILD_INTERFACE:snk::compiler_warnings>)
# The engine version a project's engine_version is compared with; one source of truth with the SDK package.
target_compile_definitions(project_system PRIVATE SNK_ENGINE_VERSION="${SNK_SDK_VERSION}")
