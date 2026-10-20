cmake_minimum_required(VERSION 3.16)
project(resource_system)
add_library(resource_system STATIC)
add_library(snk::resource_system ALIAS resource_system)

file(GLOB_RECURSE SE_RESOURCE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_RESOURCE_SOURCES})
snk_component_headers(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::engine_logger PRIVATE snk::image PUBLIC Boost::json PUBLIC Boost::asio PRIVATE snk::config_system)

#assimp
# ASSIMP_* / BUILD_SHARED_LIBS options live in cmake/ThirdParty.cmake (EPIC-41 US-41-1).
# Nothing in resource_system includes assimp: this only brings the `assimp` target in for scene and the editor.
if(SNK_WITH_ASSIMP)
	add_subdirectory(${THIRD_PARTY_PATH}/assimp assimp)
endif()
