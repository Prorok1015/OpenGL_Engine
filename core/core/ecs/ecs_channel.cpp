#include <snk/ecs/ecs_channel.hpp>

void ecs::blackboard_data::write(std::string_view name, entt::id_type type, std::string_view type_name,
								 std::shared_ptr<const void> value, int64_t writer)
{
	if (const auto committed = m_committed.find(name); committed != m_committed.end()) {
		const bool same = committed->second.type == type;
		ASSERT_MSG(same, "blackboard key '{}' holds {} but is written as {}", name, committed->second.type_name, type_name);
		if (!same) return;
	}

	const auto pending = m_pending.find(name);
	if (pending == m_pending.end()) {
		m_pending.emplace(std::string{ name }, bb_entry{ type, type_name, std::move(value), writer, false });
		return;
	}

	bb_entry& entry = pending->second;
	const bool same = entry.type == type;
	ASSERT_MSG(same, "blackboard key '{}' is written as {} and as {} in one frame", name, entry.type_name, type_name);
	if (!same) return;

	if (entry.writer == writer) {
		entry.value = std::move(value);
		return;
	}
	if (!entry.conflict_warned) {
		egLOG_WARN("scene", "blackboard key '{}' written by contexts {} and {} in one frame, the greater id wins", name, entry.writer, writer);
		entry.conflict_warned = true;
	}
	if (writer > entry.writer) {
		entry.value = std::move(value);
		entry.writer = writer;
	}
}

const void* ecs::blackboard_data::find(std::string_view name, entt::id_type type, std::string_view type_name) const
{
	const auto it = m_committed.find(name);
	if (it == m_committed.end()) return nullptr;
	const bool same = it->second.type == type;
	ASSERT_MSG(same, "blackboard key '{}' holds {} but is read as {}", name, it->second.type_name, type_name);
	return same ? it->second.value.get() : nullptr;
}

void ecs::blackboard_data::commit()
{
	for (auto& [key, entry] : m_pending) {
		entry.conflict_warned = false;
		m_committed.insert_or_assign(key, std::move(entry));
	}
	m_pending.clear();
}

void ecs::blackboard_data::clear()
{
	m_committed.clear();
	m_pending.clear();
}
