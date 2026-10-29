#pragma once
#include <snk/common/common.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/gui/gui_backend_interface.h>
#include <snk/gui/gui_menu_layout.h>
#include <snk/gui/gui_layer_interface.h>
#include <snk/common/snk_api.h>
#include <functional>

namespace gui
{
	class platform_backend;

	class SNK_API gui_system
	{
	public:
		// The two halves of the ImGui integration: renderer (draws) and platform (input / window state).
		gui_system(gui::imgui_backend_interface* backend, gui::platform_backend* platform);
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
		// An ImGui frame that draws nothing and updates no layer: `body` may use ImGui (a modal, a progress window)
		// while there is no display to show it (--headless). Not inside render().
		void run_without_drawing(const std::function<void()>& body);

	private:
		bool is_show = true;
		bool is_input_enabled = true;
		float m_pending_scale = 0.f;
		float m_applied_scale = 0.f;
		std::vector<std::shared_ptr<gui::layer_interface>> layers;
		gui::imgui_backend_interface* backend = nullptr;
		gui::platform_backend* m_platform = nullptr;
	};
}