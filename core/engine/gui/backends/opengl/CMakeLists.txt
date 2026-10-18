cmake_minimum_required(VERSION 3.16)
project(gui_gl_backend)
add_library(gui_gl_backend STATIC)
add_library(snk::gui_gl_backend ALIAS gui_gl_backend)
file(GLOB_RECURSE SE_GUI_GL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_GUI_GL_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Core ImGui sources are built by the reusable `imgui` target. Only the renderer backend impl
# (which depends on glad) is compiled here; the platform half is gui::platform_backend.
target_sources(${PROJECT_NAME} PRIVATE
"${THIRD_PARTY_PATH}/imgui/backends/imgui_impl_opengl3.cpp"
)

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::gui_backend_interface PRIVATE imgui PRIVATE snk::drv_interface PRIVATE snk::drv_opengl)