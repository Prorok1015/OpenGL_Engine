#include "gui_gl_backend.h"
#include <imgui.h>
#include <imgui_internal.h>

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "rnd_gl_texture.h"

void gui::gl::gl_imgui_backend::init(void* context)
{
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow*>(context), true);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    // TODO: change to load from memory
    ImGui::LoadIniSettingsFromDisk(get_settings_filename().c_str());
}

void gui::gl::gl_imgui_backend::new_frame()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuiID dockspace_id = ImGui::GetID("MainDockView");
    ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport());
    if (is_first_init)
    {
        is_first_init = false;
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);
        ImGuiID right = 0;
        auto left = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.2f, nullptr, &right);
        ImGuiID down = 0;
        auto up = ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.05f, nullptr, &down);

        ImGui::DockBuilderDockWindow("Common stats", up);
        ImGui::DockBuilderDockWindow("Observer", down);
        ImGui::DockBuilderDockWindow("Scene", right);
        ImGui::DockBuilderFinish(dockspace_id);
    }
}

void gui::gl::gl_imgui_backend::render()
{
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void gui::gl::gl_imgui_backend::shutdown()
{
    // TODO: change to save to memory
    ImGui::SaveIniSettingsToDisk(get_settings_filename().c_str());
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void gui::gl::gl_imgui_backend::set_display_size(int width, int height)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
}

void gui::gl::gl_imgui_backend::set_input(float mouseX, float mouseY, bool mouseButtons[3], float mouseWheel) {
    ImGuiIO& io = ImGui::GetIO();
    io.MousePos = ImVec2(mouseX, mouseY);
    for (int i = 0; i < 3; ++i)
        io.MouseDown[i] = mouseButtons[i];
    io.MouseWheel = mouseWheel;
}


ImTextureID gui::gl::gl_imgui_backend::get_imgui_texture_from_texture(rnd::driver::texture_interface* txm)
{
    if (!txm) {
        return 0;
    }

    auto* gl_txm = static_cast<rnd::driver::gl::texture*>(txm);

    return gl_txm->get_id();
}