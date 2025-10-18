cmake_minimum_required(VERSION 3.16)
project(config_system)
add_library(config_system STATIC)

file(GLOB_RECURSE SE_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_GAME_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(${THIRD_PARTY_PATH}/yaml yaml-cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE yaml-cpp PRIVATE Boost::program_options)