#pragma once
#include <snk/gui/gui_layer_interface.h>
#include <snk/gui/gui_menu_layout.h>
#include "edt_dockspace.h"
#include "edt_panel_manager.h"
#include <chrono>

namespace edt {
	class editor_layer : public gui::layer_interface {
	public:
		using UI_CALLBACK = gui::menu_layout_manager::UI_CALLBACK;

		editor_layer() {
			m_manager.set_show_main_menu(true);
		}

		virtual void on_update(std::chrono::duration<float> dt) override;

		void register_implicit(const std::string_view id, UI_CALLBACK callback) {
			m_manager.register_implicit(id, callback);
		}

		void unregister_implicit(const std::string_view id) {
			m_manager.unregister_implicit(id);
		}

		panel_manager& get_panel_manager() { return m_panel_manager; }
		dockspace& get_dockspace() { return m_dockspace; }

	private:
		gui::menu_layout_manager m_manager;
		dockspace m_dockspace;
		panel_manager m_panel_manager;
	};
}