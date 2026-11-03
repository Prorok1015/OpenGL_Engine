#include "edt_commit_graph.h"
#include <algorithm>

void edt::vcs::commit_graph::append(std::span<const graph_commit> commits)
{
	m_rows.reserve(m_rows.size() + commits.size());
	for (const graph_commit& commit : commits) {
		graph_row row;
		std::vector<std::size_t> waiting;
		std::vector<std::size_t> passing;
		for (std::size_t i = 0; i < m_lanes.size(); ++i) {
			if (!m_lanes[i]) continue;
			if (*m_lanes[i] == commit.hash) {
				waiting.push_back(i);
			} else {
				passing.push_back(i);
			}
		}
		// The leftmost lane that waited for it carries on; the others merge into it here.
		const std::size_t lane = waiting.empty() ? free_lane() : waiting.front();
		for (const std::size_t i : waiting) {
			row.up.emplace_back(i, lane);
			m_lanes[i].reset();
		}
		for (const std::size_t i : passing) {
			row.up.emplace_back(i, i);
			row.down.emplace_back(i, i);
		}
		std::sort(row.up.begin(), row.up.end());

		for (std::size_t k = 0; k < commit.parents.size(); ++k) {
			const std::string& parent = commit.parents[k];
			// A lane already waits for it (a branch merged back, or a duplicate parent): join that lane.
			if (const auto existing = lane_waiting_for(parent)) {
				// The first parent pulls a lane on its right into the commit's own: a branch merged back ends there,
				// and the line it came from bends in.
				if (k == 0 && !m_lanes[lane] && *existing > lane) {
					m_lanes[lane] = parent;
					m_lanes[*existing].reset();
					std::replace(row.down.begin(), row.down.end(), lane_link{ *existing, *existing }, lane_link{ *existing, lane });
					row.down.emplace_back(lane, lane);
				} else {
					row.down.emplace_back(lane, *existing);
				}
				continue;
			}
			// The first parent goes straight on; the others open the lowest free lane.
			const std::size_t target = k == 0 && !m_lanes[lane] ? lane : free_lane();
			m_lanes[target] = parent;
			row.down.emplace_back(lane, target);
		}
		std::sort(row.down.begin(), row.down.end());

		row.lane = lane;
		row.width = lane + 1;
		for (const auto& [from, to] : row.up) row.width = std::max({ row.width, from + 1, to + 1 });
		for (const auto& [from, to] : row.down) row.width = std::max({ row.width, from + 1, to + 1 });
		while (!m_lanes.empty() && !m_lanes.back()) m_lanes.pop_back();
		m_rows.push_back(std::move(row));
	}
}

std::size_t edt::vcs::commit_graph::open_lanes() const
{
	return static_cast<std::size_t>(std::count_if(m_lanes.begin(), m_lanes.end(), [](const auto& lane) { return lane.has_value(); }));
}

std::size_t edt::vcs::commit_graph::free_lane()
{
	for (std::size_t i = 0; i < m_lanes.size(); ++i) {
		if (!m_lanes[i]) return i;
	}
	m_lanes.emplace_back();
	return m_lanes.size() - 1;
}

std::optional<std::size_t> edt::vcs::commit_graph::lane_waiting_for(const std::string& hash) const
{
	for (std::size_t i = 0; i < m_lanes.size(); ++i) {
		if (m_lanes[i] && *m_lanes[i] == hash) return i;
	}
	return std::nullopt;
}
