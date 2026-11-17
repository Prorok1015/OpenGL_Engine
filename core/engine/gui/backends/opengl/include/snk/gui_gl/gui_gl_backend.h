#pragma once
#include <snk/gui/gui_backend_interface.h>
#include <snk/gui/gui_theme.h>
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
        // Before init() it is the look the first frame has; after it, the next new_frame() applies it. Empty text
        // font bytes: the built-in ImGui font. `fallback_font` fills the glyphs the text font lacks. Without
        // `fonts_changed` only the style changes and the atlas (and the bytes it points into) stays.
        void set_theme(const gui::theme_style& style, std::vector<std::byte> text_font,
            std::vector<std::byte> fallback_font, std::vector<std::byte> icon_font, bool fonts_changed = true);
        void rebuild_fonts(float scale) override;
        virtual void new_frame() override;
        virtual void render() override;
        virtual void shutdown() override;
    private:
        void apply_pending_theme();

        struct pending_theme {
            gui::theme_style style;
            std::vector<std::byte> text_font;
            std::vector<std::byte> fallback_font;
            std::vector<std::byte> icon_font;
            bool fonts_changed = true;
        };

        float m_dpi_scale = 1.f;
        bool m_initialized = false;
        gui::theme_style m_style;
        std::unique_ptr<pending_theme> m_pending_theme;
        gui::ui_font m_ui_font;
        // Unscaled style, so a scale change re-applies ScaleAllSizes from scratch instead of compounding.
        std::unique_ptr<ImGuiStyle> m_base_style;
    };
}