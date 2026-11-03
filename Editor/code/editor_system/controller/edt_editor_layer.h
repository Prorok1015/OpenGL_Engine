#pragma once
#include <snk/gui/gui_layer_interface.h>
#include <snk/gui/gui_menu_layout.h>
#include "edt_dockspace.h"
#include <chrono>
#include <functional>
#include <string_view>

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

		// The implicit callbacks alone, without the dockspace (--headless).
		void process_implicit() { m_manager.process_implicit(); }
		// A callback registered under an id starting with `prefix` waits for the next UI frame.
		bool has_implicit_prefix(std::string_view prefix) const {
			return m_manager.has_implicit_prefix(prefix);
		}

		// The registries the dockspace draws; nothing is drawn before this is set.
		void set_context(editor_context& context) { m_context = &context; }
		// See dockspace::set_menu_bar_status.
		void set_menu_bar_status(std::function<void()> draw) { m_dockspace.set_menu_bar_status(std::move(draw)); }
		// Game mode (#294): while `active` says so, `draw` is drawn instead of the dockspace and its panels (which is
		// kept alive, so its docked windows stay docked), and only the shortcuts of the actions `shortcuts` accepts fire.
		// `after_implicit` runs after the implicit callbacks of a frame `draw` drew (they may open a popup).
		void set_game_layout(std::function<bool()> active, std::function<void()> draw,
			std::function<bool(std::string_view)> shortcuts, std::function<void()> after_implicit) {
			m_game_active = std::move(active);
			m_draw_game = std::move(draw);
			m_game_shortcuts = std::move(shortcuts);
			m_game_after_implicit = std::move(after_implicit);
		}

	private:
		gui::menu_layout_manager m_manager;
		dockspace m_dockspace;
		editor_context* m_context = nullptr;
		std::function<bool()> m_game_active;
		std::function<void()> m_draw_game;
		std::function<bool(std::string_view)> m_game_shortcuts;
		std::function<void()> m_game_after_implicit;
	};
}