cmake_minimum_required(VERSION 3.16)
project(project_migration)

# EPIC-58: scan, plan (and later apply) of a project's data migration. Outside snk_engine and the SDK: the editor, its
# tools and the tests use it, a game never does (#752).
add_library(project_migration STATIC)
add_library(snk::project_migration ALIAS project_migration)

file(GLOB SE_PROJECT_MIGRATION_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(project_migration PRIVATE ${SE_PROJECT_MIGRATION_SOURCES})

snk_component_headers(project_migration)

target_link_libraries(project_migration PUBLIC snk::engine PUBLIC snk::engine_api PRIVATE $<BUILD_LOCAL_INTERFACE:snk::compiler_warnings>)
