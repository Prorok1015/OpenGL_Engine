cmake_minimum_required(VERSION 3.16)

# Boost (FetchContent + slim component set) is configured in cmake/ThirdParty.cmake,
# included once at the root before this directory is added (EPIC-41 US-41-1).

#components
add_subdirectory("core")
add_subdirectory("engine")

