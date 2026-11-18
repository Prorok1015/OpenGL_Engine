#include <snk/gui_gl/gui_gl_backend.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "backends/imgui_impl_opengl3.h"

#include <filesystem>
#include <system_error>

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

    gui::apply_theme(m_style, ImGui::GetStyle());
    m_base_style = std::make_unique<ImGuiStyle>(ImGui::GetStyle());

    if (m_ui_font.has_bytes() && !m_ui_font.usable()) {
        egLOG_WARN("gui", "UI font data is not a TrueType/OpenType font, using the built-in ImGui font");
    }
    // Apply OS DPI scale so the UI looks correct on high-DPI monitors
    if (m_dpi_scale > 1.f || m_ui_font.usable() || m_ui_font.has_icons()) {
        m_ui_font.add_faces_to(*io.Fonts, floorf(m_style.font_px * m_dpi_scale));
    }
    if (m_dpi_scale > 1.f) {
        ImGui::GetStyle().ScaleAllSizes(m_dpi_scale);
    }
    // ImGui keeps it across frames while no platform monitors are filled (UpdateViewportsNewFrame).
    ImGui::GetMainViewport()->DpiScale = m_dpi_scale;

    ImGui_ImplOpenGL3_Init("#version 330 core");
    // TODO: change to load from memory
    if (!get_settings_filename().empty()) {
        ImGui::LoadIniSettingsFromDisk(get_settings_filename().c_str());
    }
    m_initialized = true;
}

void gui::gl::gl_imgui_backend::set_theme(const gui::theme_style& style, std::vector<std::byte> text_font,
    std::vector<std::byte> fallback_font, std::vector<std::byte> icon_font, std::vector<std::byte> bold_font,
    std::vector<std::byte> mono_font, bool fonts_changed)
{
    if (!m_initialized) {
        m_style = style;
        if (fonts_changed) {
            m_ui_font.set_bytes(std::move(text_font));
            m_ui_font.set_fallback_bytes(std::move(fallback_font));
            m_ui_font.set_icon_bytes(std::move(icon_font));
            m_ui_font.set_bold_bytes(std::move(bold_font));
            m_ui_font.set_mono_bytes(std::move(mono_font));
        }
        return;
    }
    // A style change must not drop the fonts of a theme that has not reached a frame yet.
    if (!fonts_changed && m_pending_theme && m_pending_theme->fonts_changed) {
        m_pending_theme->style = style;
        return;
    }
    m_pending_theme = std::make_unique<pending_theme>(pending_theme{ style, std::move(text_font),
        std::move(fallback_font), std::move(icon_font), std::move(bold_font), std::move(mono_font), fonts_changed });
}

void gui::gl::gl_imgui_backend::apply_pending_theme()
{
    if (!m_pending_theme) {
        return;
    }
    const std::unique_ptr<pending_theme> theme = std::move(m_pending_theme);
    m_style = theme->style;
    if (theme->fonts_changed) {
        // The atlas points into the old bytes: it is cleared before they are replaced.
        ImGuiIO& io = ImGui::GetIO();
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        io.Fonts->Clear();
        m_ui_font.set_bytes(std::move(theme->text_font));
        m_ui_font.set_fallback_bytes(std::move(theme->fallback_font));
        m_ui_font.set_icon_bytes(std::move(theme->icon_font));
        m_ui_font.set_bold_bytes(std::move(theme->bold_font));
        m_ui_font.set_mono_bytes(std::move(theme->mono_font));
        m_ui_font.add_faces_to(*io.Fonts, floorf(m_style.font_px * m_dpi_scale));
    }

    gui::apply_theme(m_style, *m_base_style);
    ImGui::GetStyle() = *m_base_style;
    ImGui::GetStyle().ScaleAllSizes(m_dpi_scale);
    ImGui::GetMainViewport()->DpiScale = m_dpi_scale;
}

void gui::gl::gl_imgui_backend::rebuild_fonts(float scale)
{
    if (!m_base_style || scale <= 0.f) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    io.Fonts->Clear();
    m_ui_font.add_faces_to(*io.Fonts, floorf(m_style.font_px * scale));
    // ImGui_ImplOpenGL3_NewFrame recreates the texture from the new atlas.

    ImGui::GetStyle() = *m_base_style;
    ImGui::GetStyle().ScaleAllSizes(scale);
    m_dpi_scale = scale;
    ImGui::GetMainViewport()->DpiScale = scale;
}

void gui::gl::gl_imgui_backend::new_frame()
{
    apply_pending_theme();
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
    if (!get_settings_filename().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(fs_paths::path_from_utf8(get_settings_filename()).parent_path(), ec);
        ImGui::SaveIniSettingsToDisk(get_settings_filename().c_str());
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
}
