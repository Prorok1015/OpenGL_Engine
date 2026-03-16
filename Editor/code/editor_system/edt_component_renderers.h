#pragma once

namespace edt { class inspector_panel; }

namespace edt
{
	// Registers all desc-driven component renderers into the inspector panel.
	// Call once after the panel is constructed, before levels are loaded.
	void register_desc_component_renderers(inspector_panel& panel);
}
