cmake_minimum_required(VERSION 3.16)

# Boost (FetchContent + slim component set) is configured in cmake/ThirdParty.cmake,
# included once at the root before this directory is added (EPIC-41 US-41-1).

#components
add_subdirectory("core")
add_subdirectory("engine")
# The model import library (#1108), the only owner of assimp; the engine does not link it.
if(SNK_WITH_ASSIMP)
	add_subdirectory("model_import")
endif()

