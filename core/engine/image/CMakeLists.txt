cmake_minimum_required(VERSION 3.16)
project(image)
add_library(${PROJECT_NAME} STATIC)
add_library(snk::image ALIAS ${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE "image.cpp")
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(${THIRD_PARTY_PATH}/stb_image stb_image)

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE stb_image PRIVATE snk::engine_logger)
