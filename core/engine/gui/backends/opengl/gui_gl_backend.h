#pragma once
#include <snk/gui/gui_backend_interface.h>
#include <memory>

struct ImGuiStyle;

namespace gui::gl
{
    class gl_imgui_backend : public gui::imgui_backend_interface {
    public:
        gl_imgui_backend();
        ~gl_imgui_backend() override;
        virtual void init() override;
        void set_dpi_scale(float scale) override { m_dpi_scale = scale; }
        void rebuild_fonts(float scale) override;
        virtual void new_frame() override;
        virtual void render() override;
        virtual void shutdown() override;
        virtual ImTextureID get_imgui_texture_from_texture(rnd::driver::texture_interface* txm) override;
    private:
        float m_dpi_scale = 1.f;
        // Unscaled style, so a scale change re-applies ScaleAllSizes from scratch instead of compounding.
        std::unique_ptr<ImGuiStyle> m_base_style;
    };
}