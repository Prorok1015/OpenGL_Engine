cmake_minimum_required(VERSION 3.16)

project(color_text_edit)

# goossens/ImGuiColorTextEdit (MIT), the syntax-highlighting text editor widget of the GLSL editor (#1202). It lives in
# lib3dparty/ImGuiColorTextEdit as a git submodule pinned to e09ea63 (2025-06-29): the last commit that builds against the
# pinned Dear ImGui (1.91.7); newer ones need ImGui 1.92 (ImGuiStyle::FontScaleDpi, ImGuiPlatformImeData::WantTextInput).
# Move the pin only together with the ImGui submodule. The upstream ships no CMakeLists, so the target is defined here,
# in a tracked sibling folder (the same arrangement as stb_image and glad).
set(_cte_dir "${CMAKE_CURRENT_SOURCE_DIR}/../ImGuiColorTextEdit")
if(NOT EXISTS "${_cte_dir}/TextEditor.cpp")
	message(FATAL_ERROR "lib3dparty/ImGuiColorTextEdit is empty: run `git submodule update --init --recursive`")
endif()

add_library(color_text_edit STATIC "${_cte_dir}/TextEditor.cpp")
add_library(color_text_edit::color_text_edit ALIAS color_text_edit)

# SYSTEM: <TextEditor.h> in project code must not report warnings (-Werror / /WX).
target_include_directories(color_text_edit SYSTEM PUBLIC "${_cte_dir}")
# Only the headers of Dear ImGui: its code is in snk_engine (one ImGui context shared with the editor), the final link of
# whoever uses this library resolves the calls there.
target_include_directories(color_text_edit SYSTEM PRIVATE "$<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>")
target_compile_features(color_text_edit PUBLIC cxx_std_17)

# Third-party code is not held to the project's warning level.
if(MSVC)
	target_compile_options(color_text_edit PRIVATE /W0)
else()
	target_compile_options(color_text_edit PRIVATE -w)
endif()

if(SNK_ENGINE_SHARED AND MSVC)
	# imgui is exported by snk_engine (see snk_apply_target_defaults).
	target_compile_definitions(color_text_edit PRIVATE "IMGUI_API=__declspec(dllimport)")
endif()
