cmake_minimum_required(VERSION 3.16)
project(core)
add_library(${PROJECT_NAME} STATIC)
add_library(snk::core ALIAS ${PROJECT_NAME})

file(GLOB SE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_SOURCES})
# PRIVATE: the umbrella provides object code but no longer re-exports its components' headers.
# Cross-layer consumers compile against snk::core_api instead (EPIC-49 US-49-1/US-49-3).
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::config_system PRIVATE snk::application PRIVATE snk::window PRIVATE snk::resource_system PRIVATE snk::desc_system PRIVATE snk::ecs)

# Public contract of the CORE layer (EPIC-49 US-49-3). INTERFACE-only: it carries the curated
# set of components whose headers upper layers legitimately compile against — NOT drv_opengl
# (concrete GL impl). Consumers link snk::core (object code) AND snk::core_api (headers).
add_library(core_api INTERFACE)
add_library(snk::core_api ALIAS core_api)
# snk_usage_only: headers/defines only when the components live in the shared snk_engine.
snk_usage_only(_core_api_libs
	snk::common
	snk::application
	snk::ecs
	snk::desc_system
	snk::resource_system
	snk::core_input
	snk::drv_interface
	snk::window
	snk::config_system
)
target_link_libraries(core_api INTERFACE ${_core_api_libs})
# core_module.h (snk/core/) belongs to the layer contract.
snk_component_headers(core_api)


# Memory allocators (mem_*.cpp) now live in the `common` STATIC lib (EPIC-49 US-49-2).

# snk_api.h (SNK_API) for leaf components that do not link `common`: logger, config.
add_library(snk_api INTERFACE)
add_library(snk::api ALIAS snk_api)
target_include_directories(snk_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/common/include)

add_subdirectory("common")
add_subdirectory("config")
add_subdirectory("application")
add_subdirectory("input")
add_subdirectory("windows/window_service")
add_subdirectory("desc")
add_subdirectory("resource")
add_subdirectory("ecs")
add_subdirectory("video/drivers/impls/opengl")
add_subdirectory("video/drivers/impls/null")
add_subdirectory("video/drivers/interface")
