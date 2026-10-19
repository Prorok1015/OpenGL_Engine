#include "edt_editor_layer.h"
#include <snk/editor/edt_editor_context.h>

void edt::editor_layer::on_update(std::chrono::duration<float> dt)
{
	if (m_context) m_dockspace.render(m_context->panels(), m_context->actions());
	// Обрабатываем legacy-callbacks (file dialogs и прочие per-frame вещи) без menu bar
	m_manager.set_show_main_menu(false);
	m_manager.process();
}
