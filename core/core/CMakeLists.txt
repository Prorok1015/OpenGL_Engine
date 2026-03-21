cmake_minimum_required(VERSION 3.16)
project(core)
add_library(${PROJECT_NAME} STATIC)

file(GLOB SE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB SE_SOURCES_H CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
target_sources(${PROJECT_NAME} PRIVATE ${SE_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PUBLIC common PUBLIC config_system PUBLIC application PUBLIC window PUBLIC resource_system PUBLIC desc_system PUBLIC ecs)

if(NOT ENGINE_PROFILE_MODE STREQUAL "NONE")
  target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common/eng_profiler.cpp")
endif()

# TODO: have to change something. Common isn't static lib. sources must be addad somewhere. 
# Maybe common should be static lib and linked to core? Or just add sources to core? 
# For now I just add sources to core, but it isn't good solution.
target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common/mem_allocator.cpp")
target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common/mem_debug_resource.cpp")
target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common/mem_linear_allocator.cpp")
target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common/mem_pool_allocator.cpp")
target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common/mem_stack_allocator.cpp")

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
