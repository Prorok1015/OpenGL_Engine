#pragma once
#include <snk/editor/edt_inspector_registry.h>
#include <vector>

namespace edt
{
	// Registers all desc-driven component renderers into the registry.
	// Call once during editor init, before levels are loaded. The renderers stay registered while the returned handles live.
	[[nodiscard]] std::vector<registration> register_desc_component_renderers(inspector_registry& registry);
}
