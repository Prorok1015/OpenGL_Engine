cmake_minimum_required(VERSION 3.16)
project(window)
add_library(window STATIC)
add_library(snk::window ALIAS window)

file(GLOB_RECURSE SE_WINDOW_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_WINDOW_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

#libs
# GLFW_* / BUILD_SHARED_LIBS options live in cmake/ThirdParty.cmake (EPIC-41 US-41-1).
add_subdirectory(${THIRD_PARTY_PATH}/glfw glfw)
target_compile_definitions(window PUBLIC GLFW_INCLUDE_NONE=1)

target_link_libraries(${PROJECT_NAME} PUBLIC snk::core_input)
# window no longer depends on gui (EPIC-49 US-49-9): the gui_backend_interface / gui_gl_backend
# links are removed — the GUI layer owns its imgui backend now.
target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE glfw PRIVATE glad PUBLIC snk::drv_interface PRIVATE snk::drv_opengl PRIVATE snk::engine_logger)

