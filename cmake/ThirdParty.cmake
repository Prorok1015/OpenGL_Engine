# ThirdParty.cmake — centralized third-party build configuration (EPIC-41 US-41-1).
#
# Included from the root CMakeLists.txt BEFORE any add_subdirectory, so every
# third-party submodule sees consistent options regardless of the order in which
# components pull them in via add_subdirectory(${THIRD_PARTY_PATH}/...).
#
# Previously these cache variables were scattered across component CMakeLists
# (window_service/, resource/, logger/) and BUILD_SHARED_LIBS was set twice with
# FORCE, making the effective value order-dependent.

# All engine and third-party libraries are built static.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# --- GLFW (window_service) ---
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

# --- Assimp (resource_system) ---
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
# Assimp would set a GLOBAL ccache RULE_LAUNCH_COMPILE/LINK for the whole build (project targets
# included), stacking a second ccache in front of the launcher chosen by SNK_COMPILER_CACHE.
set(ASSIMP_BUILD_USE_CCACHE OFF CACHE BOOL "" FORCE)

# --- spdlog (engine_logger) ---
# Use std::format (C++20) instead of bundled fmtlib.
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)

# --- Dear ImGui (gui backends) ---
# Defines a reusable `imgui` STATIC target (the submodule ships no CMakeLists).
include(imgui)

# --- Boost (FetchContent, 1.84.0) ---
# Slim build: only configure the sublibraries the engine actually links. The Boost
# superproject pulls in their transitive dependencies automatically.
#   Boost::json                -> resource_system, desc_system, application
#   Boost::asio                -> resource_system
#   Boost::program_options     -> config_system
#   Boost::unit_test_framework -> unit_tests   (the "test" sublibrary)
# Set ENGINE_BOOST_NO_LIB to use a system-installed Boost (find_package) instead;
# in that case core/core/common/CMakeLists.txt wires the include dirs.
if(NOT ENGINE_BOOST_NO_LIB)
    include(FetchContent)
    FetchContent_Declare(
        Boost
        URL https://github.com/boostorg/boost/releases/download/boost-1.84.0/boost-1.84.0.tar.xz
        URL_MD5 893b5203b862eb9bbd08553e24ff146a
        DOWNLOAD_EXTRACT_TIMESTAMP ON
    )
    set(BOOST_INCLUDE_LIBRARIES json asio program_options test)
    FetchContent_MakeAvailable(Boost)
    file(GLOB_RECURSE BOOST_NATVIS_FILES CONFIGURE_DEPENDS "${boost_SOURCE_DIR}/libs/*.natvis")
    message(STATUS "Found Boost Natvis files: ${BOOST_NATVIS_FILES}")
    foreach(natvis_file ${BOOST_NATVIS_FILES})
        get_filename_component(filename "${natvis_file}" NAME)
        configure_file("${natvis_file}" "${CMAKE_SOURCE_DIR}/lib3dparty/natvis/${filename}" COPYONLY)
    endforeach()
endif()
