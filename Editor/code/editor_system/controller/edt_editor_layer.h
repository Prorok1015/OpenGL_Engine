#pragma once
#include <snk/gui/gui_layer_interface.h>
#include <snk/gui/gui_menu_layout.h>
#include "edt_dockspace.h"
#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace edt {
	class editor_context;

	// What editor_layer draws before the implicit callbacks of a frame. Every function may be null.
	struct editor_layouts
	{
		// #518: while `loading_active` says so (the start level of --game loads), `draw_loading` is drawn instead of
		// either layout and no shortcut fires.
		std::function<bool()> loading_active{};
		std::function<void()> draw_loading{};
		// Game mode (#294): see editor_layer::set_game_layout.
		std::function<bool()> game_active{};
		std::function<void()> draw_game{};
		std::function<bool(std::string_view)> game_shortcuts{};
		// #470: while a modal dialog of dialog_service waits for its answer no shortcut fires (Ctrl+S would save under the
		// question). Null: as without it. The agent's question window is not modal and does not count.
		std::function<bool()> modal_active{};
	};

	enum class drawn_layout { editor, game, loading };

	// One frame of the layouts: the shortcut pass (not while `modal_active` says so), then the loading screen, the game layout or the dockspace with its
	// menu bar and panels. Without the loading screen it makes the same ImGui calls as before #518.
	drawn_layout draw_layouts(dockspace& dock, panel_registry& panels, action_registry& actions, const editor_layouts& layouts);

	inline constexpr const char* loading_screen_window = "##loading_screen";
	// "Loading <level>..." for the start level's tag.
	std::string loading_screen_text(std::string_view level);
	// One undecorated black window over the main viewport with `text` in its centre. It never comes to the front, so
	// the implicit callbacks' modals (the load's progress, dialogs) stay over it.
	void draw_loading_screen(std::string_view text);

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
			m_layouts.game_active = std::move(active);
			m_layouts.draw_game = std::move(draw);
			m_layouts.game_shortcuts = std::move(shortcuts);
			m_game_after_implicit = std::move(after_implicit);
		}
		// #518: while `active` says so, `draw` is drawn instead of the dockspace or the game layout, and no shortcut
		// fires. The implicit callbacks still draw over it. Unset (the default): the frame is as without it.
		void set_loading_screen(std::function<bool()> active, std::function<void()> draw) {
			m_layouts.loading_active = std::move(active);
			m_layouts.draw_loading = std::move(draw);
		}
		// #470: see editor_layouts::modal_active.
		void set_modal_probe(std::function<bool()> active) { m_layouts.modal_active = std::move(active); }

	private:
		gui::menu_layout_manager m_manager;
		dockspace m_dockspace;
		editor_context* m_context = nullptr;
		editor_layouts m_layouts;
		std::function<void()> m_game_after_implicit;
	};
}