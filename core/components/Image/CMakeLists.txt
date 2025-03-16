cmake_minimum_required(VERSION 3.16)
project(image)
add_library(${PROJECT_NAME} STATIC)

target_sources(${PROJECT_NAME} PRIVATE "image.cpp" "image_loader.cpp")
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(${THIRD_PARTY_PATH}/stb_image stb_image)

target_link_libraries(${PROJECT_NAME} PRIVATE common PRIVATE stb_image PRIVATE engine_logger)

project(image_test)
add_executable(image_test "test/Source.cpp")
target_compile_definitions(image_test PRIVATE TESTLIB=1)
target_link_libraries(image_test image)