cmake_minimum_required(VERSION 3.16)

# Boost (FetchContent + slim component set) is configured in cmake/ThirdParty.cmake,
# included once at the root before this directory is added (EPIC-41 US-41-1).

#components
add_subdirectory("core")
add_subdirectory("engine")


#TEST_PROJECT
project(core_exe)

add_executable(core_exe main_unified_graph_test.cpp)
target_link_libraries(core_exe engine)
set_target_properties(core_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)
