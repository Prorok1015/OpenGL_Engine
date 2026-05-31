cmake_minimum_required(VERSION 3.16)
project(core)
add_library(${PROJECT_NAME} STATIC)
add_library(snk::core ALIAS ${PROJECT_NAME})

file(GLOB SE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PUBLIC snk::common PUBLIC snk::config_system PUBLIC snk::application PUBLIC snk::window PUBLIC snk::resource_system PUBLIC snk::desc_system PUBLIC snk::ecs)

# Public contract of the CORE layer (EPIC-49 US-49-3). INTERFACE-only: it carries the curated
# set of components whose headers upper layers legitimately compile against — NOT drv_opengl
# (concrete GL impl). Consumers link snk::core (object code) AND snk::core_api (headers).
add_library(core_api INTERFACE)
add_library(snk::core_api ALIAS core_api)
target_link_libraries(core_api INTERFACE
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

if(NOT ENGINE_PROFILE_MODE STREQUAL "NONE")
  target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common/eng_profiler.cpp")
endif()

# Memory allocators (mem_*.cpp) now live in the `common` STATIC lib (EPIC-49 US-49-2).

add_subdirectory("common")
add_subdirectory("config")
add_subdirectory("application")
add_subdirectory("input")
add_subdirectory("windows/window_service")
add_subdirectory("desc")
add_subdirectory("resource")
add_subdirectory("ecs")
add_subdirectory("video/drivers/impls/opengl")
add_subdirectory("video/drivers/interface")
