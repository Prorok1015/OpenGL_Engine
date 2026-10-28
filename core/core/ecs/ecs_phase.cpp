#include <snk/ecs/ecs_phase.hpp>
#include <snk/common/logger/engine_log.h>
#include <algorithm>

bool ecs::phase_table::add_game_phase(std::string name, phase anchor, phase_side side)
{
	if (name.empty() || parse_phase(name) || contains(phase_id{ std::string_view{ name } })) {
		// contains() also sees the parent's game phases, so a layer cannot reuse their names.
		egLOG_ERROR("ecs", "game phase '{}' rejected: empty, built-in or already registered", name);
		return false;
	}
	m_game.push_back(game_phase{ std::move(name), anchor, side });
	return true;
}

bool ecs::phase_table::remove_game_phase(std::string_view name)
{
	return std::erase_if(m_game, [&](const game_phase& g) { return g.name == name; }) != 0;
}

bool ecs::phase_table::contains(const phase_id& id) const
{
	if (id.is_builtin()) return true;
	if (std::ranges::any_of(m_game, [&](const game_phase& g) { return g.name == id.name(); })) return true;
	return m_parent && m_parent->contains(id);
}

std::vector<ecs::game_phase> ecs::phase_table::all_game_phases() const
{
	std::vector<game_phase> all = m_parent ? m_parent->all_game_phases() : std::vector<game_phase>{};
	all.insert(all.end(), m_game.begin(), m_game.end());
	return all;
}

std::vector<ecs::phase_id> ecs::phase_table::ordered() const
{
	const std::vector<game_phase> game = all_game_phases();
	std::vector<phase_id> out;
	out.reserve(builtin_phase_count + game.size());
	for (phase p : builtin_phases) {
		for (const auto& g : game) {
			if (g.anchor == p && g.side == phase_side::before) out.emplace_back(std::string_view{ g.name });
		}
		out.emplace_back(p);
		for (const auto& g : game) {
			if (g.anchor == p && g.side == phase_side::after) out.emplace_back(std::string_view{ g.name });
		}
	}
	return out;
}
