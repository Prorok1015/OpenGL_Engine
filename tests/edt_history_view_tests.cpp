#include <boost/test/unit_test.hpp>
#include "edt_history_view.h"
#include <cstdint>
#include <string>
#include <vector>

// The lines the History tab draws per graph row (#457), and its short texts.
namespace {

using edt::vcs::graph_segment;
using at = graph_segment::at;

struct laid_out
{
	edt::vcs::commit_graph graph;
	std::vector<std::vector<graph_segment>> rows;

	explicit laid_out(const std::vector<edt::vcs::graph_commit>& commits)
	{
		graph.append(commits);
		graph.finish();
		std::vector<std::uint32_t> above = graph.lanes_above(0);
		for (const auto& row : graph.rows()) {
			rows.push_back(edt::vcs::row_segments(graph, row, above));
			graph.advance(above, row);
		}
	}
};

bool has(const std::vector<graph_segment>& segments, graph_segment wanted)
{
	for (const auto& s : segments) {
		if (s.from_lane == wanted.from_lane && s.from == wanted.from && s.to_lane == wanted.to_lane && s.to == wanted.to
			&& s.colour_lane == wanted.colour_lane) {
			return true;
		}
	}
	return false;
}

}

BOOST_AUTO_TEST_SUITE(EdtHistoryView)

BOOST_AUTO_TEST_CASE(ALinearHistory_IsOneLineThroughTheDots)
{
	const laid_out history({ { "c", { "b" } }, { "b", { "a" } }, { "a", {} } });
	BOOST_REQUIRE_EQUAL(history.rows.size(), 3u);
	BOOST_TEST(history.rows[0].size() == 1u);
	BOOST_TEST(has(history.rows[0], { 0, at::middle, 0, at::bottom, 0 }));
	BOOST_TEST(history.rows[1].size() == 2u);
	BOOST_TEST(has(history.rows[1], { 0, at::top, 0, at::middle, 0 }));
	BOOST_TEST(has(history.rows[1], { 0, at::middle, 0, at::bottom, 0 }));
	BOOST_TEST(history.rows[2].size() == 1u);
	BOOST_TEST(has(history.rows[2], { 0, at::top, 0, at::middle, 0 }));
}

BOOST_AUTO_TEST_CASE(AMergedBranch_JoinsTheMainLaneBelowItsDot)
{
	const laid_out history({ { "m", { "a", "f" } }, { "f", { "a" } }, { "a", {} } });
	BOOST_TEST(has(history.rows[0], { 0, at::middle, 1, at::bottom, 1 }));
	BOOST_TEST(has(history.rows[1], { 0, at::top, 0, at::bottom, 0 }));
	BOOST_TEST(has(history.rows[1], { 1, at::top, 1, at::middle, 1 }));
	BOOST_TEST(has(history.rows[1], { 1, at::middle, 0, at::bottom, 0 }));
	BOOST_TEST(history.rows[2].size() == 1u);
}

BOOST_AUTO_TEST_CASE(ALaneThatBends_RunsIntoTheDotsLaneBelow)
{
	const laid_out history({ { "a", { "c" } }, { "b", { "p" } }, { "c", { "p" } }, { "p", {} } });
	const auto& row = history.graph.rows()[2];
	BOOST_REQUIRE(row.bent == 1u);
	BOOST_TEST(has(history.rows[2], { 0, at::top, 0, at::middle, 0 }));
	BOOST_TEST(has(history.rows[2], { 1, at::top, 0, at::bottom, 1 }));
	BOOST_TEST(has(history.rows[2], { 0, at::middle, 0, at::bottom, 0 }));
}

BOOST_AUTO_TEST_CASE(TheAge_IsShortWhileRecent_ThenADate)
{
	const std::int64_t now = 1700000000;
	BOOST_TEST(edt::vcs::age_text(now, now - 30) == "just now");
	BOOST_TEST(edt::vcs::age_text(now, now - 300) == "5 min ago");
	BOOST_TEST(edt::vcs::age_text(now, now - 3 * 3600) == "3 h ago");
	BOOST_TEST(edt::vcs::age_text(now, now - 86400) == "1 day ago");
	BOOST_TEST(edt::vcs::age_text(now, now - 2 * 86400) == "2 days ago");
	BOOST_TEST(edt::vcs::age_text(now, now - 40 * 86400) == "2023-10-05");
	BOOST_TEST(edt::vcs::age_text(now, now + 600) == "2023-11-14");
	BOOST_TEST(edt::vcs::age_text(now, 0) == "1970-01-01");
}

BOOST_AUTO_TEST_CASE(AShortHash_IsItsFirstEightCharacters)
{
	BOOST_TEST(edt::vcs::short_hash("0123456789abcdef") == "01234567");
	BOOST_TEST(edt::vcs::short_hash("abc") == "abc");
}

BOOST_AUTO_TEST_SUITE_END()
