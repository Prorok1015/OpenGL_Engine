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
    set(BOOST_INCLUDE_LIBRARIES json hana program_options)
    FetchContent_MakeAvailable(Boost)
endif()

#components
add_subdirectory("core/common")
add_subdirectory("core/config")
add_subdirectory("core/common/logger")
add_subdirectory("core/application")
add_subdirectory("core/windows")
add_subdirectory("core/desc")
add_subdirectory("core/resource")
add_subdirectory("core/ecs")
add_subdirectory("engine/input")
add_subdirectory("engine/gui/backends/interface")
add_subdirectory("engine/gui/backends/opengl")
add_subdirectory("engine/gui/gui_system")
add_subdirectory("engine/render/drivers/impls/opengl")
add_subdirectory("engine/render/drivers/interface")
add_subdirectory("engine/render/render_system")
add_subdirectory("engine/game_system")
add_subdirectory("engine/scene")
add_subdirectory("engine/image")


#TEST_PROJECT
project(core_exe)
set(CMAKE_CXX_STANDARD 20)

add_executable(core_exe main.cpp)
target_link_libraries(core_exe application)
set_target_properties(core_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)
