# imgui.cmake — reusable Dear ImGui STATIC target (EPIC-41 US-41-6).
#
# Dear ImGui lives in lib3dparty/imgui as a git submodule and upstream ships no
# CMakeLists.txt. Rather than add a build file inside the submodule (which the parent
# repo cannot track), the target is defined here, in a parent-tracked module included
# from ThirdParty.cmake. Requires THIRD_PARTY_PATH to be set by the root CMakeLists.
#
# Backend implementations (imgui_impl_glfw.cpp, imgui_impl_opengl3.cpp) depend on
# glfw/glad and are compiled by gui_gl_backend, not here.

add_library(imgui STATIC
	"${THIRD_PARTY_PATH}/imgui/imgui.cpp"
	"${THIRD_PARTY_PATH}/imgui/imgui_draw.cpp"
	"${THIRD_PARTY_PATH}/imgui/imgui_widgets.cpp"
	"${THIRD_PARTY_PATH}/imgui/imgui_tables.cpp"
	"${THIRD_PARTY_PATH}/imgui/imgui_demo.cpp"
	"${THIRD_PARTY_PATH}/imgui/misc/cpp/imgui_stdlib.cpp"
)
add_library(imgui::imgui ALIAS imgui)

target_include_directories(imgui PUBLIC
	"${THIRD_PARTY_PATH}/imgui"
	"${THIRD_PARTY_PATH}/imgui/misc/cpp"
)

if(SNK_ENGINE_SHARED AND MSVC)
	target_compile_definitions(imgui PRIVATE "IMGUI_API=__declspec(dllexport)")
endif()
