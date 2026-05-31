cmake_minimum_required(VERSION 3.16)
project(gui_gl_backend)
add_library(gui_gl_backend STATIC)
add_library(snk::gui_gl_backend ALIAS gui_gl_backend)
file(GLOB_RECURSE SE_GUI_GL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_GUI_GL_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Core ImGui sources are built by the reusable `imgui` target. Only the platform/renderer
# backend impls (which depend on glfw/glad) are compiled here (EPIC-41 US-41-6).
target_sources(${PROJECT_NAME} PRIVATE
"${THIRD_PARTY_PATH}/imgui/backends/imgui_impl_glfw.cpp"
"${THIRD_PARTY_PATH}/imgui/backends/imgui_impl_opengl3.cpp"
)

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::gui_backend_interface PRIVATE imgui PRIVATE glfw PRIVATE snk::drv_interface PRIVATE snk::drv_opengl)