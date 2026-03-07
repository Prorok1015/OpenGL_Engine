cmake_minimum_required(VERSION 3.16)
set(CMAKE_CXX_STANDARD 20)

if(NOT ENGINE_BOOST_NO_LIB)
    include(FetchContent)
    FetchContent_Declare(
    Boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.84.0/boost-1.84.0.tar.xz
    URL_MD5 893b5203b862eb9bbd08553e24ff146a
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    )
    set(BOOST_INCLUDE_LIBRARIES json hana program_options asio test)
    FetchContent_MakeAvailable(Boost)
    file(GLOB_RECURSE BOOST_NATVIS_FILES "${boost_SOURCE_DIR}/libs/*.natvis")
    message(STATUS "Found Boost Natvis files: ${BOOST_NATVIS_FILES}")
    foreach(natvis_file ${BOOST_NATVIS_FILES})
        get_filename_component(filename "${natvis_file}" NAME)
        configure_file("${natvis_file}" "${CMAKE_SOURCE_DIR}/lib3dparty/natvis/${filename}" COPYONLY)
    endforeach()
endif()

#components
add_subdirectory("core")
add_subdirectory("engine")


#TEST_PROJECT
project(core_exe)
set(CMAKE_CXX_STANDARD 20)

add_executable(core_exe main_unified_graph_test.cpp)
target_link_libraries(core_exe engine)
set_target_properties(core_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)
