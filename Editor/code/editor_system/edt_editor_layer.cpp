#include "edt_editor_layer.h"

void edt::editor_layer::on_update(std::chrono::duration<float> dt)
{
	m_dockspace.render(m_panel_manager);
	// Обрабатываем legacy-callbacks (file dialogs и прочие per-frame вещи) без menu bar
	m_manager.set_show_main_menu(false);
	m_manager.process();
}
