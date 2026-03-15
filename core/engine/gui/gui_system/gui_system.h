#pragma once
#include "gui_renderer.h"
#include "gui_menu_layout.h"
#include "gui_layer_interface.h"

namespace gui
{
	class gui_system
	{
	public:
		gui_system(gui::imgui_backend_interface* backend);
		~gui_system();

		gui_system(gui_system&&) = delete;
		gui_system& operator= (gui_system&&) = delete;

		gui_system(const gui_system&) = delete;
		gui_system& operator= (const gui_system&) = delete;

		void render_menues();

		void set_show_ui(bool show) { is_show = show; }
		bool is_hiden() const { return !is_show; }

		//void switch_input(inp::KEYBOARD_BUTTONS code, inp::KEY_ACTION action);
		void set_is_input_enabled(bool enable);

		void push_layer(std::shared_ptr<gui::layer_interface> layer) {
			layers.push_back(layer);
			layer->on_attach();
		}

		void pop_layer(std::shared_ptr<gui::layer_interface> layer) {
			layers.erase(std::remove(layers.begin(), layers.end(), layer));
			layer->on_dettach();
		}

		gui::imgui_backend_interface* get_backend_interface() const {
			return backend;
		}

		void render();

	private:
		bool is_show = true;
		bool is_input_enabled = true;
		std::vector<std::shared_ptr<gui::layer_interface>> layers;
		gui::imgui_backend_interface* backend = nullptr;
	};

	gui_system& get_system();
}