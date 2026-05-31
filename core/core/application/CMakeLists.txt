cmake_minimum_required(VERSION 3.16)
project(application)
add_library(application STATIC)
add_library(snk::application ALIAS application)

file(GLOB_RECURSE SE_APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_APP_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common)
# Boost::json is not referenced by any application header or source (the app_data_storage
# the EPIC-41 AC assumed no longer exists) — keep it PRIVATE, not PUBLIC. Candidate for
# removal as a follow-up; left linked here to stay strictly behavior-preserving.
target_link_libraries(${PROJECT_NAME} PRIVATE Boost::json)
