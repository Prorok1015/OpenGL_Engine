#include "scn_node_index.h"
#include <algorithm>

scn::node_index& scn::node_index::get(entt::registry& reg)
{
	if (auto* existing = reg.ctx().find<node_index>()) {
		return *existing;
	}

	auto& index = reg.ctx().emplace<node_index>();
	reg.on_construct<node_id_component>().connect<&node_index::on_set>(index);
	reg.on_update<node_id_component>().connect<&node_index::on_set>(index);
	reg.on_destroy<node_id_component>().connect<&node_index::on_destroy>(index);

	for (auto [e, comp] : reg.view<node_id_component>().each()) {
		index.on_set(reg, e);
	}
	return index;
}

entt::entity scn::node_index::find(std::span<const uint64_t> path) const
{
	auto [first, last] = m_entries.equal_range(hash_path(path));
	for (auto it = first; it != last; ++it) {
		if (std::ranges::equal(it->second.path, path)) {
			return it->second.entity;
		}
	}
	return entt::null;
}

uint64_t scn::node_index::hash_path(std::span<const uint64_t> path)
{
	uint64_t h = 0xcbf29ce484222325ull ^ path.size();
	for (uint64_t id : path) {
		h ^= id + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	}
	return h;
}

void scn::node_index::on_set(entt::registry& reg, entt::entity e)
{
	remove(e); // on_update: drop the stale path first
	const auto& path = reg.get<node_id_component>(e).path;
	const uint64_t h = hash_path(path);
	m_entries.emplace(h, entry{ e, path });
	m_hash_of[e] = h;
}

void scn::node_index::on_destroy(entt::registry&, entt::entity e)
{
	remove(e);
}

void scn::node_index::remove(entt::entity e)
{
	auto it = m_hash_of.find(e);
	if (it == m_hash_of.end()) {
		return;
	}
	auto [first, last] = m_entries.equal_range(it->second);
	for (auto entry_it = first; entry_it != last; ++entry_it) {
		if (entry_it->second.entity == e) {
			m_entries.erase(entry_it);
			break;
		}
	}
	m_hash_of.erase(it);
}
