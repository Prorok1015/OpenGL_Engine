cmake_minimum_required(VERSION 3.16)
project(gui)
add_library(gui STATIC)
add_library(snk::gui ALIAS gui)
file(GLOB_RECURSE SE_GUI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${SE_GUI_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# gui_backend_interface is PUBLIC: gui_system.h's public API returns gui::imgui_backend_interface*
# (get_backend_interface). gui_gl_backend is PRIVATE: gui_init constructs the concrete GL backend
# now that window no longer owns it (EPIC-49 US-49-9).
target_link_libraries(${PROJECT_NAME} PRIVATE snk::common PRIVATE snk::window PRIVATE snk::desc_system PRIVATE snk::resource_system PRIVATE snk::render PUBLIC snk::gui_backend_interface PRIVATE snk::gui_gl_backend)