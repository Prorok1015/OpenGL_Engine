#pragma once
// Avoid pulling all of <imgui.h> into this widely-included interface header (EPIC-49 US-49-4).
// ImTextureID is an imgui integer typedef (ImU64 == unsigned long long in imgui v1.91.x).
// Re-declaring it to the identical underlying type is legal even when <imgui.h> is also
// included in the same TU. Keep in sync with imgui.h if the imgui version changes.
using ImTextureID = unsigned long long;
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_handle.h>
#include <string>

namespace gui
{
    class imgui_backend_interface {
    public:
        // `drv` is the driver whose textures the panels show; it must outlive the backend.
        explicit imgui_backend_interface(rnd::driver::driver_interface& drv) : m_driver(drv) {}
        virtual ~imgui_backend_interface() {}

        // Renderer half of the ImGui integration: owns the ImGui context. Input and window state
        // come from gui::platform_backend.
        virtual void init() = 0;
        virtual void set_dpi_scale(float scale) {}
        // Rebuilds fonts and re-applies style scaling for a new content scale. Call outside an ImGui frame.
        virtual void rebuild_fonts(float scale) {}
        // Renderer per-frame setup only; gui_system calls ImGui::NewFrame after the platform's.
        virtual void new_frame() = 0;
        virtual void render() = 0;
        virtual void shutdown() = 0;
        // What ImGui::Image takes for the texture behind `texture`: the name the driver's API gives it, which the
        // renderer half draws with. 0 for a null, stale or foreign handle (ImGui then draws nothing useful, and the
        // caller should have asked whether the target exists). A panel passes the handle it got from the render
        // targets or the texture manager; the id is good for the frame it is asked in.
        virtual ImTextureID to_imgui(rnd::driver::texture_handle texture) const {
            return static_cast<ImTextureID>(m_driver.native_texture_name(texture));
        }
        void set_initial_layout_by_default(bool flag) { is_first_init = flag; }
        const std::string& get_settings_filename() const {
            return settings_layout_file_name;
        }
        void set_settings_filename(const std::string& filename) {
            settings_layout_file_name = filename;
        }
    protected:
        rnd::driver::driver_interface& m_driver;
        bool is_first_init = true;
        std::string settings_layout_file_name = "layout_settings.ini";
    };
}