#pragma once
#include "gui_backend_interface.h"

namespace gui::gl
{
    class gl_imgui_backend : public gui::imgui_backend_interface {
    public:
        virtual void init(void* context) override;
        virtual void new_frame() override;
        virtual void render() override;
        virtual void shutdown() override;
        virtual void set_display_size(int width, int height) override;
        virtual void set_input(float mouseX, float mouseY, bool mouseButtons[3], float mouseWheel) override;
        virtual ImTextureID get_imgui_texture_from_texture(rnd::driver::texture_interface* txm) override;
    };
}