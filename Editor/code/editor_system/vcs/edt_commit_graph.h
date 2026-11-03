#pragma once
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The lanes of the commit graph in the Git panel (#457): which column each commit sits in and the lines between rows.
// Built page by page as vcs.log delivers commits, children before parents (git's --date-order or --topo-order); the
// state carries over, so a page never makes earlier rows change and nothing is laid out twice. A row costs O(log of
// the open lanes) and keeps only what changes at it: the lanes that merely pass a row are not stored, lanes_above()
// rebuilds them from a snapshot every k_snapshot_every rows. Pure, no ImGui.
namespace edt::vcs
{
	struct graph_commit
	{
		std::string hash;
		// First parent first, as git lists them.
		std::vector<std::string> parents;
	};

	inline constexpr std::uint32_t k_no_lane = 0xFFFFFFFFu;

	struct graph_row
	{
		// The lane of the commit's dot.
		std::uint32_t lane = 0;
		// A line comes into the dot from above (a child of this commit was laid out before it).
		bool from_above = false;
		// Lanes the dot links to below, one per parent laid out (see links()); a lane already waiting for that parent
		// is joined, so two links never go to one lane.
		std::uint32_t links_begin = 0;
		std::uint32_t links_count = 0;
		// A lane that ends at this row and bends into `lane` below: it waited for the commit's first parent, which now
		// goes on in the commit's own lane. k_no_lane: none.
		std::uint32_t bent = k_no_lane;
		// Parents that got no link: over max_lanes, or already laid out above (input out of order).
		std::uint32_t dropped = 0;
		// Lanes the row draws: every lane it uses is below it.
		std::uint32_t width = 0;
	};

	class commit_graph
	{
	public:
		static constexpr std::size_t k_snapshot_every = 256;

		// Lays out the next commits; their rows follow the ones already there. A commit no lane waits for opens one (a
		// branch tip). A commit seen before is ignored, and so is a parent listed twice. A parent that was already laid
		// out (a parent before its child: not a child-before-parent order) gets no link.
		void append(std::span<const graph_commit> commits);
		// The history ends here (the last page, or a shallow or filtered log): lanes still waiting for a parent that will
		// not come are closed; their lines run to the bottom of the last row. A finished graph takes no more commits (a
		// new history or filter is a new graph).
		void finish();
		// At most this many lanes open at once; a parent that would open one more gets no link (graph_row::dropped).
		void set_max_lanes(std::size_t lanes) { m_max_lanes = lanes == 0 ? 1 : lanes; }

		const std::vector<graph_row>& rows() const { return m_rows; }
		std::span<const std::uint32_t> links(const graph_row& row) const;
		// The lanes with a line at the top of `row` (a commit below waits on each), ascending.
		std::vector<std::uint32_t> lanes_above(std::size_t row) const;
		// From the lanes above `row` to the lanes above the next row.
		void advance(std::vector<std::uint32_t>& lanes, const graph_row& row) const;
		// Lanes that wait for a commit not seen yet (after the last row).
		std::size_t open_lanes() const { return m_waiting.size(); }
		// Lane operations done so far: a check that the cost follows the commits, not the open lanes.
		std::size_t work() const { return m_work; }

	private:
		std::uint32_t take_lane();
		void free_lane(std::uint32_t lane);
		void wait(std::uint32_t lane, const std::string& hash);
		void snapshot();

		// Per lane, the commit it waits for (empty: free); trailing free lanes are dropped.
		std::vector<std::string> m_lanes;
		std::unordered_map<std::string, std::uint32_t> m_waiting;
		std::set<std::uint32_t> m_free;
		std::unordered_set<std::string> m_seen;
		std::vector<graph_row> m_rows;
		std::vector<std::uint32_t> m_links;
		// lanes_above() of every k_snapshot_every-th row.
		std::vector<std::vector<std::uint32_t>> m_snapshots;
		std::size_t m_max_lanes = 4096;
		std::size_t m_work = 0;
		bool m_finished = false;
	};
}
