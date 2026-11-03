#include <boost/test/unit_test.hpp>
#include "edt_history_view.h"
#include <cstdint>
#include <format>
#include <string>
#include <vector>

// The lines the History tab draws per graph row (#457), and its short texts.
namespace {

using edt::vcs::graph_segment;
using at = graph_segment::at;

struct laid_out
{
	edt::vcs::commit_graph graph;
	std::vector<std::vector<std::uint32_t>> above;
	std::vector<std::vector<graph_segment>> rows;

	explicit laid_out(const std::vector<edt::vcs::graph_commit>& commits)
	{
		graph.append(commits);
		graph.finish();
		std::vector<std::uint32_t> lanes = graph.lanes_above(0);
		for (const auto& row : graph.rows()) {
			above.push_back(lanes);
			rows.push_back(edt::vcs::row_segments(graph, row, lanes));
			graph.advance(lanes, row);
		}
	}
};

std::string text_of(const std::vector<graph_segment>& segments)
{
	const auto name = [](at height) { return height == at::top ? "top" : height == at::middle ? "mid" : "bot"; };
	std::string out;
	for (const auto& s : segments) {
		out += std::format("({},{}->{},{}:{})", s.from_lane, name(s.from), s.to_lane, name(s.to), s.colour_lane);
	}
	return out;
}

}

BOOST_AUTO_TEST_SUITE(EdtHistoryView)

BOOST_AUTO_TEST_CASE(ALinearHistory_IsOneLineThroughTheDots)
{
	const laid_out history({ { "c", { "b" } }, { "b", { "a" } }, { "a", {} } });
	BOOST_REQUIRE_EQUAL(history.rows.size(), 3u);
	BOOST_TEST(text_of(history.rows[0]) == "(0,mid->0,bot:0)");
	BOOST_TEST(text_of(history.rows[1]) == "(0,top->0,mid:0)(0,mid->0,bot:0)");
	BOOST_TEST(text_of(history.rows[2]) == "(0,top->0,mid:0)");
}

BOOST_AUTO_TEST_CASE(AMergedBranch_JoinsTheMainLaneBelowItsDot)
{
	const laid_out history({ { "m", { "a", "f" } }, { "f", { "a" } }, { "a", {} } });
	BOOST_TEST(text_of(history.rows[0]) == "(0,mid->0,bot:0)(0,mid->1,bot:1)");
	BOOST_TEST(text_of(history.rows[1]) == "(0,top->0,bot:0)(1,top->1,mid:1)(1,mid->0,bot:0)");
	BOOST_TEST(text_of(history.rows[2]) == "(0,top->0,mid:0)");
}

BOOST_AUTO_TEST_CASE(ARowWithALineFromAbove_ABentLane_AndALink)
{
	const laid_out history({ { "a", { "c" } }, { "b", { "p" } }, { "c", { "p" } }, { "p", {} } });
	BOOST_REQUIRE(history.graph.rows()[2].bent == 1u);
	BOOST_TEST(text_of(history.rows[0]) == "(0,mid->0,bot:0)");
	BOOST_TEST(text_of(history.rows[1]) == "(0,top->0,bot:0)(1,mid->1,bot:1)");
	BOOST_TEST(text_of(history.rows[2]) == "(0,top->0,mid:0)(1,top->0,bot:1)(0,mid->0,bot:0)");
	BOOST_TEST(text_of(history.rows[3]) == "(0,top->0,mid:0)");
}

BOOST_AUTO_TEST_CASE(AnOctopusMerge_FansOutAndComesBack)
{
	const laid_out history({ { "o", { "a", "b", "c" } }, { "a", { "r" } }, { "b", { "r" } }, { "c", { "r" } }, { "r", {} } });
	BOOST_TEST(text_of(history.rows[0]) == "(0,mid->0,bot:0)(0,mid->1,bot:1)(0,mid->2,bot:2)");
	BOOST_TEST(text_of(history.rows[1]) == "(0,top->0,mid:0)(1,top->1,bot:1)(2,top->2,bot:2)(0,mid->0,bot:0)");
	BOOST_TEST(text_of(history.rows[2]) == "(0,top->0,bot:0)(1,top->1,mid:1)(2,top->2,bot:2)(1,mid->0,bot:0)");
	BOOST_TEST(text_of(history.rows[3]) == "(0,top->0,bot:0)(2,top->2,mid:2)(2,mid->0,bot:0)");
	BOOST_TEST(text_of(history.rows[4]) == "(0,top->0,mid:0)");
}

BOOST_AUTO_TEST_CASE(AParentPastTheEnd_RunsToTheBottomOfTheLastRow)
{
	const laid_out history({ { "c", { "b" } }, { "b", { "a" } } });
	BOOST_TEST(history.graph.open_lanes() == 0u);
	BOOST_TEST(text_of(history.rows[1]) == "(0,top->0,mid:0)(0,mid->0,bot:0)");
}

BOOST_AUTO_TEST_CASE(TheLanesAboveARow_AreTheSameFromASnapshotAsFromTheTop)
{
	constexpr std::size_t count = 600;
	std::vector<edt::vcs::graph_commit> commits;
	for (std::size_t i = 0; i < count; ++i) {
		edt::vcs::graph_commit commit{ std::to_string(i), {} };
		if (i + 1 < count) commit.parents.push_back(std::to_string(i + 1));
		if (i % 7 == 0 && i + 3 < count) commit.parents.push_back(std::to_string(i + 3));
		commits.push_back(std::move(commit));
	}
	const laid_out history(commits);
	for (const std::size_t row : { std::size_t{ 1 }, std::size_t{ 255 }, std::size_t{ 256 }, std::size_t{ 257 },
			 std::size_t{ 300 }, std::size_t{ 511 }, std::size_t{ 512 }, std::size_t{ 599 } }) {
		const std::vector<std::uint32_t> from_snapshot = history.graph.lanes_above(row);
		BOOST_TEST(from_snapshot == history.above[row], boost::test_tools::per_element());
		BOOST_TEST(text_of(edt::vcs::row_segments(history.graph, history.graph.rows()[row], from_snapshot))
			== text_of(history.rows[row]));
	}
}

BOOST_AUTO_TEST_CASE(TheAge_IsShortWhileRecent_ThenADate)
{
	const std::int64_t now = 1700000000;
	BOOST_TEST(edt::vcs::age_text(now, now) == "just now");
	BOOST_TEST(edt::vcs::age_text(now, now - 59) == "just now");
	BOOST_TEST(edt::vcs::age_text(now, now - 60) == "1 min ago");
	BOOST_TEST(edt::vcs::age_text(now, now - 3599) == "59 min ago");
	BOOST_TEST(edt::vcs::age_text(now, now - 3600) == "1 h ago");
	BOOST_TEST(edt::vcs::age_text(now, now - 86399) == "23 h ago");
	BOOST_TEST(edt::vcs::age_text(now, now - 86400) == "1 day ago");
	BOOST_TEST(edt::vcs::age_text(now, now - 2 * 86400) == "2 days ago");
	BOOST_TEST(edt::vcs::age_text(now, now - 30 * 86400 + 1) == "29 days ago");
	BOOST_TEST(edt::vcs::age_text(now, now - 30 * 86400) == "2023-10-15");
	BOOST_TEST(edt::vcs::age_text(now, now + 1) == "2023-11-14");
	BOOST_TEST(edt::vcs::age_text(now, 0) == "1970-01-01");
}

BOOST_AUTO_TEST_CASE(AShortHash_IsItsFirstEightCharacters)
{
	BOOST_TEST(edt::vcs::short_hash("0123456789abcdef") == "01234567");
	BOOST_TEST(edt::vcs::short_hash("abc") == "abc");
}

BOOST_AUTO_TEST_SUITE_END()
