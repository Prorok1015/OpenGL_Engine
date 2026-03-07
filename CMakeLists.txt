cmake_minimum_required(VERSION 3.16)
project(build_all)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THIRD_PARTY_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib3dparty")
set(ENGINE_CORE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/core")
set(SE_PROJECT_SOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(Editor)

enable_testing()

file(GLOB TEST_SOURCES "unittests/*.cpp")
list(REMOVE_ITEM TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/unittests/unit_tests.cpp")

add_executable(unit_tests unittests/unit_tests.cpp ${TEST_SOURCES})

target_link_libraries(unit_tests PRIVATE engine Boost::unit_test_framework)

add_test(NAME unit_tests COMMAND unit_tests)
