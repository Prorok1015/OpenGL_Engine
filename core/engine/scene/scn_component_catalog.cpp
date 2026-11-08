#include <snk/scene/scn_component_catalog.h>
#include <snk/common/logger/engine_log.h>

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
	const component_entry& stored = m_entries.emplace_back(std::move(entry));
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
	auto* entry = const_cast<component_entry*>(it->second);
	m_by_id.erase(entry->id);
	m_by_name.erase(it);
	entry->name.clear();
	entry->schema.reset();
	return true;
}

const scn::component_entry* scn::component_catalog::find(std::string_view name) const
{
	if (const auto it = m_by_name.find(std::string{ name }); it != m_by_name.end()) {
		return it->second;
	}
	return m_parent ? m_parent->find(name) : nullptr;
}

const scn::component_entry* scn::component_catalog::find(entt::id_type id) const
{
	if (const auto it = m_by_id.find(id); it != m_by_id.end()) {
		return it->second;
	}
	return m_parent ? m_parent->find(id) : nullptr;
}

std::vector<scn::unregistered_storage> scn::unregistered_storages(const entt::registry& reg, const component_catalog& catalog)
{
	std::vector<unregistered_storage> result;
	for (const auto [id, storage] : reg.storage()) {
		if (storage.empty() || catalog.find(id)) {
			continue;
		}
		result.push_back({ id, std::string{ storage.type().name() } });
	}
	return result;
}
