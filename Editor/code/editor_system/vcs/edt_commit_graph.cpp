#include "edt_commit_graph.h"
#include <snk/common/engine_assert.h>
#include <algorithm>

void edt::vcs::commit_graph::append(std::span<const graph_commit> commits)
{
	ASSERT_MSG(!m_finished, "commit_graph::append after finish(): lay out a new history in a new graph");
	if (m_finished) return;
	m_rows.reserve(m_rows.size() + commits.size());
	for (const graph_commit& commit : commits) {
		++m_work;
		if (!m_seen.insert(commit.hash).second) continue;
		if (m_rows.size() % k_snapshot_every == 0) snapshot();

		graph_row row;
		const auto width_before = static_cast<std::uint32_t>(m_lanes.size());
		std::uint32_t lane = 0;
		if (const auto waiting = m_waiting.find(commit.hash); waiting != m_waiting.end()) {
			lane = waiting->second;
			row.from_above = true;
			m_waiting.erase(waiting);
			m_lanes[lane].clear();
		} else {
			lane = take_lane();
		}
		row.lane = lane;
		row.links_begin = static_cast<std::uint32_t>(m_links.size());
		bool lane_used = false;
		std::uint32_t widest = std::max(width_before, lane + 1);

		for (std::size_t k = 0; k < commit.parents.size(); ++k) {
			++m_work;
			const std::string& parent = commit.parents[k];
			if (std::find(commit.parents.begin(), commit.parents.begin() + static_cast<std::ptrdiff_t>(k), parent)
				!= commit.parents.begin() + static_cast<std::ptrdiff_t>(k)) {
				continue;
			}
			if (m_seen.contains(parent)) {
				++row.dropped;
				continue;
			}
			std::uint32_t target = k_no_lane;
			if (const auto waiting = m_waiting.find(parent); waiting != m_waiting.end()) {
				const std::uint32_t other = waiting->second;
				// The first parent pulls a lane on its right into the commit's own: a branch merged back ends there.
				if (k == 0 && !lane_used && other > lane) {
					m_waiting.erase(waiting);
					free_lane(other);
					wait(lane, parent);
					lane_used = true;
					row.bent = other;
					widest = std::max(widest, other + 1);
					target = lane;
				} else {
					target = other;
				}
			} else if (!lane_used) {
				wait(lane, parent);
				lane_used = true;
				target = lane;
			} else if (m_lanes.size() - m_free.size() < m_max_lanes) {
				target = take_lane();
				wait(target, parent);
			} else {
				++row.dropped;
				continue;
			}
			m_links.push_back(target);
			widest = std::max(widest, target + 1);
		}
		if (!lane_used) free_lane(lane);
		row.links_count = static_cast<std::uint32_t>(m_links.size()) - row.links_begin;
		row.width = widest;
		m_rows.push_back(row);
	}
}

void edt::vcs::commit_graph::finish()
{
	m_waiting.clear();
	m_lanes.clear();
	m_free.clear();
	m_finished = true;
}

std::span<const std::uint32_t> edt::vcs::commit_graph::links(const graph_row& row) const
{
	return std::span<const std::uint32_t>(m_links).subspan(row.links_begin, row.links_count);
}

std::vector<std::uint32_t> edt::vcs::commit_graph::lanes_above(std::size_t row) const
{
	if (row >= m_rows.size() || m_snapshots.empty()) return {};
	const std::size_t first = row / k_snapshot_every;
	std::vector<std::uint32_t> lanes = m_snapshots[first];
	for (std::size_t r = first * k_snapshot_every; r < row; ++r) advance(lanes, m_rows[r]);
	return lanes;
}

void edt::vcs::commit_graph::advance(std::vector<std::uint32_t>& lanes, const graph_row& row) const
{
	const auto drop = [&lanes](std::uint32_t lane) {
		const auto at = std::lower_bound(lanes.begin(), lanes.end(), lane);
		if (at != lanes.end() && *at == lane) lanes.erase(at);
	};
	if (row.from_above) drop(row.lane);
	if (row.bent != k_no_lane) drop(row.bent);
	for (const std::uint32_t lane : links(row)) {
		const auto at = std::lower_bound(lanes.begin(), lanes.end(), lane);
		if (at == lanes.end() || *at != lane) lanes.insert(at, lane);
	}
}

std::uint32_t edt::vcs::commit_graph::take_lane()
{
	++m_work;
	if (!m_free.empty()) {
		const std::uint32_t lane = *m_free.begin();
		m_free.erase(m_free.begin());
		return lane;
	}
	m_lanes.emplace_back();
	return static_cast<std::uint32_t>(m_lanes.size() - 1);
}

void edt::vcs::commit_graph::free_lane(std::uint32_t lane)
{
	++m_work;
	m_lanes[lane].clear();
	m_free.insert(lane);
	// Trailing free lanes go, so the width shrinks when the rightmost branches end.
	while (!m_lanes.empty() && m_free.contains(static_cast<std::uint32_t>(m_lanes.size() - 1))) {
		m_free.erase(static_cast<std::uint32_t>(m_lanes.size() - 1));
		m_lanes.pop_back();
	}
}

void edt::vcs::commit_graph::wait(std::uint32_t lane, const std::string& hash)
{
	m_lanes[lane] = hash;
	m_waiting[hash] = lane;
}

void edt::vcs::commit_graph::snapshot()
{
	std::vector<std::uint32_t> lanes;
	lanes.reserve(m_waiting.size());
	for (const auto& [hash, lane] : m_waiting) lanes.push_back(lane);
	std::sort(lanes.begin(), lanes.end());
	m_snapshots.push_back(std::move(lanes));
}
