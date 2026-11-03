#pragma once
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The lanes of the commit graph in the Git panel (#457): which column each commit sits in and the lines between rows.
// Built page by page as vcs.log delivers commits, children before parents (git's --date-order or --topo-order); the
// state carries over, so a page never makes earlier rows change and nothing is laid out twice. Pure, no ImGui.
namespace edt::vcs
{
	struct graph_commit
	{
		std::string hash;
		// First parent first, as git lists them.
		std::vector<std::string> parents;
	};

	// One segment between two lanes: drawn from `from` to `to`.
	using lane_link = std::pair<std::size_t, std::size_t>;

	struct graph_row
	{
		// The lane of the commit's dot.
		std::size_t lane = 0;
		// From the row's top edge to the dot's height: a lane that passes (x -> x) or comes into the commit (x -> lane).
		std::vector<lane_link> up;
		// From the dot's height to the row's bottom edge: a lane that passes (x -> x) or goes from the commit to a
		// parent's lane (lane -> y).
		std::vector<lane_link> down;
		// Lanes the row draws: every lane index in `up` and `down` is below it.
		std::size_t width = 0;

		bool operator==(const graph_row&) const = default;
	};

	class commit_graph
	{
	public:
		// Lays out the next commits; their rows follow the ones already there. A commit no lane waits for opens a lane
		// (a branch tip). Out of order input (a parent before its child) still lays out, only less tidily: the child's
		// lane runs on.
		void append(std::span<const graph_commit> commits);
		const std::vector<graph_row>& rows() const { return m_rows; }
		// Lanes that wait for a commit not seen yet (after the last row).
		std::size_t open_lanes() const;

	private:
		std::size_t free_lane();
		std::optional<std::size_t> lane_waiting_for(const std::string& hash) const;

		// Per lane, the commit it waits for; nothing: the lane is free.
		std::vector<std::optional<std::string>> m_lanes;
		std::vector<graph_row> m_rows;
	};
}
