#pragma once
#include "level/scn_prefab_desc.h"
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace edt {

// Registry of optional custom ImGui renderers for component descs.
// Types without a registered renderer are handled by the generic JSON renderer.
// Registration uses T::__type constants — no string literals needed at registration sites.
class component_ui_registry {
public:
	using renderer_fn = std::function<bool(scn::prefab_desc::prefab_comp_node&)>;

	// Register a renderer for desc type T.
	// fn is instantiated with prefab_comp_node& here — for typed access use T::__type key
	// and down-cast inside the lambda if needed.
	template<typename T>
	void register_renderer(renderer_fn fn)
	{
		m_renderers[std::string(T::__type)] = std::move(fn);
	}

	// Invoke the registered renderer for the given type_name.
	// Returns nullopt if no renderer is registered (caller should use generic fallback).
	// Returns the renderer's bool return value (true = data changed).
	std::optional<bool> invoke(std::string_view type_name, scn::prefab_desc::prefab_comp_node& comp) const
	{
		auto it = m_renderers.find(std::string(type_name));
		if (it == m_renderers.end())
			return std::nullopt;
		return it->second(comp);
	}

private:
	std::unordered_map<std::string, renderer_fn> m_renderers;
};

} // namespace edt
