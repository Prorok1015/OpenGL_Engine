#include "gui_gl_backend.h"
#include <imgui.h>
#include <imgui_internal.h>

#include "backends/imgui_impl_opengl3.h"

gui::gl::gl_imgui_backend::gl_imgui_backend(rnd::driver::driver_interface& drv)
    : imgui_backend_interface(drv)
{
}
gui::gl::gl_imgui_backend::~gl_imgui_backend() = default;

void gui::gl::gl_imgui_backend::init()
{
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    m_base_style = std::make_unique<ImGuiStyle>(ImGui::GetStyle());

    // Apply OS DPI scale so the UI looks correct on high-DPI monitors
    if (m_dpi_scale > 1.f) {
        ImFontConfig font_cfg;
        font_cfg.SizePixels = floorf(13.f * m_dpi_scale);
        io.Fonts->AddFontDefault(&font_cfg);
        ImGui::GetStyle().ScaleAllSizes(m_dpi_scale);
    }

    ImGui_ImplOpenGL3_Init("#version 330 core");
    // TODO: change to load from memory
    ImGui::LoadIniSettingsFromDisk(get_settings_filename().c_str());
}

void gui::gl::gl_imgui_backend::rebuild_fonts(float scale)
{
    if (!m_base_style || scale <= 0.f) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    io.Fonts->Clear();
    ImFontConfig font_cfg;
    font_cfg.SizePixels = floorf(13.f * scale);
    io.Fonts->AddFontDefault(&font_cfg);
    // ImGui_ImplOpenGL3_NewFrame recreates the texture from the new atlas.

    ImGui::GetStyle() = *m_base_style;
    ImGui::GetStyle().ScaleAllSizes(scale);
    m_dpi_scale = scale;
}

void gui::gl::gl_imgui_backend::new_frame()
{
    ImGui_ImplOpenGL3_NewFrame();
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
    ImGui::DestroyContext();
}
