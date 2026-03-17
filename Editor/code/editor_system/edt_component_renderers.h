#pragma once

namespace edt { class component_ui_registry; }

namespace edt
{
	// Registers all desc-driven component renderers into the registry.
	// Call once during editor init, before levels are loaded.
	void register_desc_component_renderers(component_ui_registry& registry);
}
