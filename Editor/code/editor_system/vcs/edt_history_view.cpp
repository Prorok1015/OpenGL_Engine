#include "edt_history_view.h"
#include <chrono>
#include <format>

std::vector<edt::vcs::graph_segment> edt::vcs::row_segments(const commit_graph& graph, const graph_row& row,
	std::span<const std::uint32_t> above)
{
	using at = graph_segment::at;
	std::vector<graph_segment> segments;
	segments.reserve(above.size() + row.links_count + 1);
	for (const std::uint32_t lane : above) {
		if (row.from_above && lane == row.lane) {
			segments.push_back({ lane, at::top, lane, at::middle, lane });
		} else if (lane == row.bent) {
			segments.push_back({ lane, at::top, row.lane, at::bottom, lane });
		} else {
			segments.push_back({ lane, at::top, lane, at::bottom, lane });
		}
	}
	for (const std::uint32_t target : graph.links(row)) {
		segments.push_back({ row.lane, at::middle, target, at::bottom, target });
	}
	return segments;
}

std::string edt::vcs::age_text(std::int64_t now_seconds, std::int64_t time_seconds)
{
	const std::int64_t age = now_seconds - time_seconds;
	if (age >= 0) {
		if (age < 60) return "just now";
		if (age < 60 * 60) return std::format("{} min ago", age / 60);
		if (age < 24 * 60 * 60) return std::format("{} h ago", age / (60 * 60));
		if (age < 30 * 24 * 60 * 60) {
			const std::int64_t days = age / (24 * 60 * 60);
			return std::format("{} day{} ago", days, days == 1 ? "" : "s");
		}
	}
	const std::chrono::sys_seconds when{ std::chrono::seconds{ time_seconds } };
	const std::chrono::year_month_day day{ std::chrono::floor<std::chrono::days>(when) };
	return std::format("{:04}-{:02}-{:02}", static_cast<int>(day.year()), static_cast<unsigned>(day.month()),
		static_cast<unsigned>(day.day()));
}

std::string_view edt::vcs::short_hash(std::string_view hash)
{
	return hash.substr(0, 8);
}
