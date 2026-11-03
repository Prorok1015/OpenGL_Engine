#include "edt_editor_layer.h"
#include "edt_main_menu.h"
#include <snk/editor/edt_editor_context.h>

void edt::editor_layer::on_update(std::chrono::duration<float> dt)
{
	if (m_context) {
		// Before either layout, so a shortcut also works without the menu bar; an action that switches the layout
		// takes effect in this frame.
		fire_shortcuts(m_context->actions());
		if (m_game_active && m_draw_game && m_game_active()) m_draw_game();
		else m_dockspace.render(m_context->panels(), m_context->actions());
	}
	// Обрабатываем legacy-callbacks (file dialogs и прочие per-frame вещи) без menu bar
	m_manager.set_show_main_menu(false);
	m_manager.process();
}
