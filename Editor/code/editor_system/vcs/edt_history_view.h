#pragma once
#include "edt_commit_graph.h"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What the History tab draws for one row of the commit graph, and its short texts. Pure, no ImGui.
namespace edt::vcs
{
	struct graph_segment
	{
		// Heights inside the row: its top edge, the dot's centre, its bottom edge.
		enum class at : std::uint8_t { top, middle, bottom };
		std::uint32_t from_lane = 0;
		at from = at::top;
		std::uint32_t to_lane = 0;
		at to = at::bottom;
		// The lane whose colour the line takes.
		std::uint32_t colour_lane = 0;
	};

	// The lines of `row`, given the lanes with a line at its top (commit_graph::lanes_above, then advance from row to
	// row): lanes that only pass, the line into the dot, the lane that bends into the dot's lane, the links to the
	// parents below. The dot itself sits at (row.lane, middle).
	std::vector<graph_segment> row_segments(const commit_graph& graph, const graph_row& row, std::span<const std::uint32_t> above);

	// "just now", "5 min ago", "3 h ago", "2 days ago", else the date (UTC) "2026-10-01". A time in the future reads
	// as its date.
	std::string age_text(std::int64_t now_seconds, std::int64_t time_seconds);

	// The first 8 characters of a commit hash.
	std::string_view short_hash(std::string_view hash);
}
