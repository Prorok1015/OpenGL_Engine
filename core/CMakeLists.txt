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
    set(BOOST_INCLUDE_LIBRARIES json hana)
    FetchContent_MakeAvailable(Boost)
endif()

#components
add_subdirectory("components/common")
add_subdirectory("components/common/logger")
add_subdirectory("components/application")
add_subdirectory("components/gui/backends/interface")
add_subdirectory("components/gui/backends/opengl")
add_subdirectory("components/gui/gui_system")
add_subdirectory("components/render/drivers/impls/opengl")
add_subdirectory("components/render/drivers/interface")
add_subdirectory("components/render/render_system")
add_subdirectory("components/input")
add_subdirectory("components/resource")
add_subdirectory("components/game_system")
add_subdirectory("components/ecs")
add_subdirectory("components/scene")
add_subdirectory("components/image")
add_subdirectory("components/windows")
add_subdirectory("components/desc")


#TEST_PROJECT
project(core_exe)
set(CMAKE_CXX_STANDARD 20)

add_executable(core_exe main.cpp)
target_link_libraries(core_exe application)
set_target_properties(core_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)
