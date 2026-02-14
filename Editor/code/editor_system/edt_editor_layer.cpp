#include "edt_editor_layer.h"

void edt::editor_layer::on_update(std::chrono::duration<float> dt)
{
	m_manager.process();
}
