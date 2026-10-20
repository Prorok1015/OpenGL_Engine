cmake_minimum_required(VERSION 3.16)
project(gui_gl_backend)
add_library(gui_gl_backend STATIC)
add_library(snk::gui_gl_backend ALIAS gui_gl_backend)
file(GLOB_RECURSE SE_GUI_GL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_GUI_GL_SOURCES})

# gui_gl_backend.h is internal: only gui (and this target) see the source dir.
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Core ImGui sources are built by the reusable `imgui` target. Only the renderer backend impl
# (which depends on glad) is compiled here; the platform half is gui::platform_backend.
target_sources(${PROJECT_NAME} PRIVATE
"${THIRD_PARTY_PATH}/imgui/backends/imgui_impl_opengl3.cpp"
)

# Third-party source inside a project target (which carries -Werror / /WX): its warnings stay visible
# but are not errors (#258).
set_source_files_properties("${THIRD_PARTY_PATH}/imgui/backends/imgui_impl_opengl3.cpp" PROPERTIES
	COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/WX-,-Wno-error>")

target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::gui_backend_interface PRIVATE imgui PRIVATE snk::drv_interface)
# No drv_opengl link: imgui_impl_opengl3 uses its embedded loader (no IMGUI_IMPL_OPENGL_LOADER_* define is set anywhere).