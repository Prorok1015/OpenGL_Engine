cmake_minimum_required(VERSION 3.16)
project(engine)
add_library(${PROJECT_NAME} STATIC)
add_library(snk::engine ALIAS ${PROJECT_NAME})

file(GLOB SE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PUBLIC snk::core PUBLIC snk::render PUBLIC snk::input_system PUBLIC snk::scene PUBLIC snk::game PUBLIC snk::gui)

add_subdirectory("input")
add_subdirectory("gui/backends/interface")
add_subdirectory("gui/backends/opengl")
add_subdirectory("gui/gui_system")
add_subdirectory("render/render_system")
add_subdirectory("game_system")
add_subdirectory("scene")
add_subdirectory("image")