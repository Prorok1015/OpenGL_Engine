#include "edt_editor_layer.h"
#include "edt_main_menu.h"
#include <snk/editor/edt_editor_context.h>

void edt::editor_layer::on_update(std::chrono::duration<float> dt)
{
	bool game_drawn = false;
	if (m_context) {
		const auto game = [this] { return m_game_active && m_draw_game && m_game_active(); };
		// Before either layout, so a shortcut also works without the menu bar; an action that switches the layout
		// takes effect in this frame. In game mode the other keys belong to the game.
		const std::function<bool(std::string_view)> any;
		fire_shortcuts(m_context->actions(), game() ? m_game_shortcuts : any);
		if (game()) {
			m_dockspace.keep_alive();
			m_draw_game();
			game_drawn = true;
		} else {
			m_dockspace.render(m_context->panels(), m_context->actions());
		}
	}
	// Обрабатываем legacy-callbacks (file dialogs и прочие per-frame вещи) без menu bar
	m_manager.set_show_main_menu(false);
	m_manager.process();
	// Input of the game window is decided only now: a popup an implicit callback opened this frame takes it.
	if (game_drawn && m_game_after_implicit) m_game_after_implicit();
}
