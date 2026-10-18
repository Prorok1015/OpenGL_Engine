#pragma once
#include <common.h>
#include <rnd_driver_interface.h>
#include "gui_backend_interface.h"
#include "gui_menu_layout.h"
#include "gui_layer_interface.h"
#include "snk_api.h"

namespace gui
{
	class SNK_API gui_system
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

		// Applied at the start of the next render(), i.e. outside an ImGui frame.
		void on_content_scale_changed(float scale) { m_pending_scale = scale; }

		void render();

	private:
		bool is_show = true;
		bool is_input_enabled = true;
		float m_pending_scale = 0.f;
		std::vector<std::shared_ptr<gui::layer_interface>> layers;
		gui::imgui_backend_interface* backend = nullptr;
	};
}