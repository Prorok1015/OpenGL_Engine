#pragma once
#include <snk/gui/gui_layer_interface.h>
#include <snk/gui/gui_menu_layout.h>
#include "edt_dockspace.h"
#include <chrono>

namespace edt {
	class editor_context;

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

		// A callback registered under an id starting with `prefix` waits for the next UI frame.
		bool has_implicit_prefix(std::string_view prefix) const {
			return m_manager.has_implicit_prefix(prefix);
		}

		// The registries the dockspace draws; nothing is drawn before this is set.
		void set_context(editor_context& context) { m_context = &context; }
		// See dockspace::set_menu_bar_status.
		void set_menu_bar_status(std::function<void()> draw) { m_dockspace.set_menu_bar_status(std::move(draw)); }

	private:
		gui::menu_layout_manager m_manager;
		dockspace m_dockspace;
		editor_context* m_context = nullptr;
	};
}