#include <snk/scene/scn_component_catalog.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>

const scn::component_entry* scn::component_catalog::add_entry(component_entry entry)
{
	if (entry.name.empty()) {
		egLOG_ERROR("scene", "component not added to the catalog: empty name");
		return nullptr;
	}
	if (find(entry.name)) {
		egLOG_ERROR("scene", "component '{}' not added to the catalog: the name is taken", entry.name);
		return nullptr;
	}
	if (const component_entry* other = find(entry.id)) {
		egLOG_ERROR("scene", "component '{}' not added to the catalog: its type is registered as '{}'", entry.name, other->name);
		return nullptr;
	}
	if (entry.schema && entry.schema->object_type && *entry.schema->object_type != *entry.object_type) {
		egLOG_ERROR("scene", "component '{}' not added to the catalog: its schema '{}' is bound to another type", entry.name,
			entry.schema->schema ? entry.schema->schema->name : std::string{});
		return nullptr;
	}
	const component_entry& stored = *m_entries.emplace_back(std::make_unique<component_entry>(std::move(entry)));
	m_by_name.emplace(stored.name, &stored);
	m_by_id.emplace(stored.id, &stored);
	return &stored;
}

bool scn::component_catalog::remove(std::string_view name)
{
	const auto it = m_by_name.find(std::string{ name });
	if (it == m_by_name.end()) {
		return false;
	}
	const component_entry* entry = it->second;
	m_by_id.erase(entry->id);
	m_by_name.erase(it);
	std::erase_if(m_entries, [entry](const std::unique_ptr<component_entry>& stored) { return stored.get() == entry; });
	return true;
}

void scn::component_catalog::clear()
{
	m_by_name.clear();
	m_by_id.clear();
	m_entries.clear();
}

bool scn::component_catalog::shadowed(const component_entry& entry) const
{
	if (!m_parent) {
		return false;
	}
	const component_entry* by_name = m_parent->find(entry.name);
	const component_entry* by_id = m_parent->find(entry.id);
	return (by_name && by_name != &entry) || (by_id && by_id != &entry);
}

const scn::component_entry* scn::component_catalog::find(std::string_view name) const
{
	if (const component_entry* inherited = m_parent ? m_parent->find(name) : nullptr) {
		return inherited;
	}
	const auto it = m_by_name.find(std::string{ name });
	return it != m_by_name.end() ? it->second : nullptr;
}

const scn::component_entry* scn::component_catalog::find(entt::id_type id) const
{
	if (const component_entry* inherited = m_parent ? m_parent->find(id) : nullptr) {
		return inherited;
	}
	const auto it = m_by_id.find(id);
	return it != m_by_id.end() ? it->second : nullptr;
}
