#include <snk/editor/edt_inspector_registry.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <utility>

struct edt::inspector_registry::entry
{
	std::uint64_t key = 0;
	std::string type;
	// Shared so that invoke() keeps the renderer alive even if it removes its own registration.
	std::shared_ptr<inspector_renderer> renderer;
};

edt::inspector_registry::inspector_registry() = default;
edt::inspector_registry::~inspector_registry() = default;

edt::registration edt::inspector_registry::add(std::string type, inspector_renderer renderer)
{
	if (type.empty() || !renderer) {
		egLOG_ERROR("Editor/Inspector", "renderer '{}' not registered: it needs a type and a renderer", type);
		return {};
	}
	if (contains(type)) {
		egLOG_ERROR("Editor/Inspector", "renderer '{}' not registered: the type already has one", type);
		return {};
	}

	auto added = std::make_shared<entry>();
	added->key = m_next_key++;
	added->type = std::move(type);
	added->renderer = std::make_shared<inspector_renderer>(std::move(renderer));
	const std::uint64_t key = added->key;
	m_entries.push_back(std::move(added));
	return registration(m_alive, [this, key] { remove_key(key); });
}

void edt::inspector_registry::remove_key(std::uint64_t key)
{
	std::erase_if(m_entries, [key](const std::shared_ptr<entry>& e) { return e->key == key; });
}

bool edt::inspector_registry::remove(std::string_view type)
{
	return std::erase_if(m_entries, [type](const std::shared_ptr<entry>& e) { return e->type == type; }) > 0;
}

bool edt::inspector_registry::contains(std::string_view type) const
{
	return find(type) != nullptr;
}

const edt::inspector_renderer* edt::inspector_registry::find(std::string_view type) const
{
	const auto it = std::find_if(m_entries.begin(), m_entries.end(),
		[type](const std::shared_ptr<entry>& e) { return e->type == type; });
	return it != m_entries.end() ? (*it)->renderer.get() : nullptr;
}

std::vector<std::string> edt::inspector_registry::types() const
{
	std::vector<std::string> out;
	out.reserve(m_entries.size());
	for (const auto& e : m_entries) out.push_back(e->type);
	return out;
}

std::optional<bool> edt::inspector_registry::invoke(std::string_view type, scn::prefab_desc::prefab_comp_node& comp) const
{
	const auto it = std::find_if(m_entries.begin(), m_entries.end(),
		[type](const std::shared_ptr<entry>& e) { return e->type == type; });
	if (it == m_entries.end()) return std::nullopt;
	const std::shared_ptr<inspector_renderer> renderer = (*it)->renderer;
	return (*renderer)(comp);
}
