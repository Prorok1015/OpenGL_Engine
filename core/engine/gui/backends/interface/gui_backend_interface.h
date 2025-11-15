#pragma once
#include <imgui.h>
#include "rnd_texture_interface.h"
#include <string>

namespace gui
{
    class imgui_backend_interface {
    public:
        virtual ~imgui_backend_interface() {}

        virtual void init(void* context = nullptr) = 0;
        virtual void new_frame() = 0;
        virtual void render() = 0;
        virtual void shutdown() = 0;
        virtual void set_display_size(int width, int height) = 0;
        virtual void set_input(float mouseX, float mouseY, bool mouseButtons[3], float mouseWheel) = 0;
        virtual ImTextureID get_imgui_texture_from_texture(rnd::driver::texture_interface* txm) = 0;
        void set_initial_layout_by_default(bool flag) { is_first_init = flag; }
        const std::string& get_settings_filename() const {
            return settings_layout_file_name;
        }
        void set_settings_filename(const std::string& filename) {
            settings_layout_file_name = filename;
        }
    protected:
        bool is_first_init = true;
        std::string settings_layout_file_name = "layout_settings.ini";
    };
}