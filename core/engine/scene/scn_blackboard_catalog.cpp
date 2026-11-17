#include <snk/scene/scn_blackboard_catalog.h>
#include <snk/common/logger/engine_log.h>

void scn::blackboard_catalog::write_value(const desc::schema_binding& schema, const void* value, json::object& out)
{
	desc::serialize(schema, value, out);
}

const scn::blackboard_type* scn::blackboard_catalog::add_entry(blackboard_type entry)
{
	if (!entry.schema) {
		egLOG_WARN("scene", "blackboard type '{}' has no schema: not registered", entry.name);
		return nullptr;
	}
	if (m_by_name.contains(entry.name) || m_by_id.contains(entry.id)) {
		egLOG_WARN("scene", "blackboard type '{}' is already registered", entry.name);
		return nullptr;
	}
	const blackboard_type* added = m_entries.emplace_back(std::make_unique<blackboard_type>(std::move(entry))).get();
	m_by_name.emplace(added->name, added);
	m_by_id.emplace(added->id, added);
	return added;
}

const scn::blackboard_type* scn::blackboard_catalog::find(std::string_view name) const
{
	const auto it = m_by_name.find(std::string{ name });
	return it == m_by_name.end() ? nullptr : it->second;
}

const scn::blackboard_type* scn::blackboard_catalog::find(entt::id_type id) const
{
	const auto it = m_by_id.find(id);
	return it == m_by_id.end() ? nullptr : it->second;
}

void scn::blackboard_catalog::clear()
{
	m_by_name.clear();
	m_by_id.clear();
	m_entries.clear();
}
