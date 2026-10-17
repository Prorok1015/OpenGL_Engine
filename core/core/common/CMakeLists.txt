cmake_minimum_required(VERSION 3.16)
project(common)
add_library(${PROJECT_NAME} STATIC)
add_library(snk::common ALIAS ${PROJECT_NAME})
# Memory allocators live here (previously compiled into `core` — see EPIC-49 US-49-2).
target_sources(${PROJECT_NAME} PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/mem_allocator.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/mem_debug_resource.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/mem_linear_allocator.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/mem_pool_allocator.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/mem_stack_allocator.cpp"
	# Always compiled: profiler entry points are referenced unconditionally (render, gui);
	# ENGINE_PROFILE_MODE only gates the PROFILE_* macros.
	"${CMAKE_CURRENT_SOURCE_DIR}/eng_profiler.cpp"
)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
#glm
add_subdirectory(${THIRD_PARTY_PATH}/glm glm)
target_compile_definitions(glm INTERFACE GLM_ENABLE_EXPERIMENTAL)
# logger (spdlog backend) — needed by engine_log.h included from common
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/logger logger)
target_link_libraries(${PROJECT_NAME} PUBLIC glm snk::engine_logger)

if(ENGINE_PROFILE_MODE STREQUAL "INTERNAL")
  target_compile_definitions(${PROJECT_NAME} PUBLIC ENGINE_PROFILE_INTERNAL=1)
elseif(ENGINE_PROFILE_MODE STREQUAL "TRACY")
  target_compile_definitions(${PROJECT_NAME} PUBLIC ENGINE_PROFILE_TRACY=1)
endif()

if(ENGINE_BOOST_NO_LIB)
  find_package(Boost REQUIRED)
  target_include_directories(${PROJECT_NAME} PUBLIC ${Boost_INCLUDE_DIRS})
  target_compile_definitions(${PROJECT_NAME} PUBLIC BOOST_ALL_NO_LIB)
  message(STATUS "Boost_INCLUDE_DIRS: ${Boost_INCLUDE_DIRS}")
  message(STATUS "Boost_LIBRARIES: ${Boost_LIBRARIES}")
endif()
