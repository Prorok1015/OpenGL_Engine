#pragma once
#include <snk/gui/gui_backend_interface.h>
#include <snk/gui/gui_ui_font.h>
#include <cstddef>
#include <vector>
#include <memory>

struct ImGuiStyle;

namespace gui::gl
{
    class gl_imgui_backend : public gui::imgui_backend_interface {
    public:
        explicit gl_imgui_backend(rnd::driver::driver_interface& drv);
        ~gl_imgui_backend() override;
        virtual void init() override;
        void set_dpi_scale(float scale) override { m_dpi_scale = scale; }
        // TTF bytes for the UI font; before init(). Empty or not a font: the built-in ImGui font stays.
        void set_ui_font_bytes(std::vector<std::byte> bytes) { m_ui_font.set_bytes(std::move(bytes)); }
        void rebuild_fonts(float scale) override;
        virtual void new_frame() override;
        virtual void render() override;
        virtual void shutdown() override;
    private:
        float m_dpi_scale = 1.f;
        gui::ui_font m_ui_font;
        // Unscaled style, so a scale change re-applies ScaleAllSizes from scratch instead of compounding.
        std::unique_ptr<ImGuiStyle> m_base_style;
    };
}