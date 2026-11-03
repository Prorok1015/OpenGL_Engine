#include <boost/test/unit_test.hpp>
#include "edt_commit_graph.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <span>
#include <string>
#include <vector>

// The lanes of the Git panel's commit graph (#457): hand-checked histories, an oracle that follows every parent from
// its child's link to its own row on random histories in two child-before-parent orders, paging, the edge cases and
// the cost with thousands of open lanes.
namespace {

using edt::vcs::commit_graph;
using edt::vcs::graph_commit;
using edt::vcs::graph_row;
using edt::vcs::k_no_lane;

struct shape
{
	std::uint32_t lane = 0;
	bool from_above = false;
	std::vector<std::uint32_t> links;
	std::uint32_t bent = k_no_lane;
	std::uint32_t width = 0;
	bool operator==(const shape&) const = default;
};

std::ostream& operator<<(std::ostream& out, const shape& s)
{
	out << "{lane " << s.lane << (s.from_above ? ", from above" : "") << ", links";
	for (const auto l : s.links) out << ' ' << l;
	if (s.bent != k_no_lane) out << ", bent " << s.bent;
	return out << ", width " << s.width << '}';
}

std::vector<shape> shapes(const commit_graph& graph)
{
	std::vector<shape> out;
	for (const graph_row& row : graph.rows()) {
		const auto links = graph.links(row);
		out.push_back({ row.lane, row.from_above, { links.begin(), links.end() }, row.bent, row.width });
	}
	return out;
}

std::vector<shape> lay_out(const std::vector<graph_commit>& commits, std::size_t page = 0)
{
	commit_graph graph;
	if (page == 0) {
		graph.append(commits);
	} else {
		for (std::size_t at = 0; at < commits.size(); at += page) {
			graph.append(std::span(commits).subspan(at, std::min(page, commits.size() - at)));
		}
	}
	return shapes(graph);
}

// A random history of `count` commits, as indices: parents of commit i are later indices (children first).
std::vector<std::vector<std::size_t>> random_history(std::size_t count, unsigned seed)
{
	std::mt19937 rng(seed);
	std::vector<std::vector<std::size_t>> parents(count);
	for (std::size_t i = 0; i + 1 < count; ++i) {
		const std::size_t room = count - i - 1;
		std::uniform_int_distribution<int> how_many(0, 9);
		const int roll = how_many(rng);
		const std::size_t wanted = roll == 0 ? 0 : roll < 7 ? 1 : roll < 9 ? 2 : 3; // roots, plain, merges, octopus
		std::uniform_int_distribution<std::size_t> near(1, std::min<std::size_t>(room, 8));
		for (std::size_t k = 0; k < wanted && k < room; ++k) {
			const std::size_t parent = i + near(rng);
			if (std::find(parents[i].begin(), parents[i].end(), parent) == parents[i].end()) parents[i].push_back(parent);
		}
	}
	return parents;
}

// Another child-before-parent order of the same history: Kahn's algorithm, picking at random among the ready ones.
std::vector<std::size_t> random_order(const std::vector<std::vector<std::size_t>>& parents, unsigned seed)
{
	std::mt19937 rng(seed);
	std::vector<std::size_t> children(parents.size(), 0);
	for (const auto& of : parents) for (const std::size_t p : of) ++children[p];
	std::vector<std::size_t> ready;
	for (std::size_t i = 0; i < parents.size(); ++i) if (children[i] == 0) ready.push_back(i);
	std::vector<std::size_t> order;
	while (!ready.empty()) {
		std::uniform_int_distribution<std::size_t> pick(0, ready.size() - 1);
		const std::size_t at = pick(rng);
		const std::size_t next = ready[at];
		ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(at));
		order.push_back(next);
		for (const std::size_t p : parents[next]) if (--children[p] == 0) ready.push_back(p);
	}
	return order;
}

std::vector<std::size_t> natural_order(std::size_t count)
{
	std::vector<std::size_t> order(count);
	for (std::size_t i = 0; i < count; ++i) order[i] = i;
	return order;
}

std::vector<graph_commit> commits_of(const std::vector<std::vector<std::size_t>>& parents, const std::vector<std::size_t>& order)
{
	std::vector<graph_commit> out;
	for (const std::size_t i : order) {
		graph_commit commit{ "c" + std::to_string(i), {} };
		for (const std::size_t p : parents[i]) commit.parents.push_back("c" + std::to_string(p));
		out.push_back(std::move(commit));
	}
	return out;
}

// Follows every parent from the link of its child to its own row: it sits in the lane the link led to (or the lane
// that lane bent into), a line comes into it from above, and no lane ever waits for two commits. lanes_above() agrees
// with the lanes the oracle knows to be waiting, row by row.
void check_with_oracle(const std::vector<graph_commit>& commits, const commit_graph& graph)
{
	const auto& rows = graph.rows();
	BOOST_TEST_REQUIRE(rows.size() == commits.size());
	std::map<std::string, std::uint32_t> waiting;
	for (std::size_t r = 0; r < rows.size(); ++r) {
		const graph_row& row = rows[r];
		const graph_commit& commit = commits[r];

		std::vector<std::uint32_t> open;
		for (const auto& [hash, lane] : waiting) open.push_back(lane);
		std::sort(open.begin(), open.end());
		BOOST_TEST_REQUIRE((std::adjacent_find(open.begin(), open.end()) == open.end())); // one commit per lane
		BOOST_TEST_REQUIRE((graph.lanes_above(r) == open));
		for (const auto lane : open) BOOST_TEST_REQUIRE(lane < row.width);

		if (const auto it = waiting.find(commit.hash); it != waiting.end()) {
			BOOST_TEST_REQUIRE(row.from_above);
			BOOST_TEST_REQUIRE(row.lane == it->second);
			waiting.erase(it);
		} else {
			BOOST_TEST_REQUIRE(!row.from_above);
		}
		const auto links = graph.links(row);
		std::size_t next_link = 0;
		for (const std::string& parent : commit.parents) {
			BOOST_TEST_REQUIRE(next_link < links.size());
			const std::uint32_t link = links[next_link++];
			BOOST_TEST_REQUIRE(link < row.width);
			if (const auto it = waiting.find(parent); it != waiting.end()) {
				if (link == row.lane && row.bent == it->second) {
					it->second = row.lane; // the lane it waited in bends into the commit's
				} else {
					BOOST_TEST_REQUIRE(link == it->second);
				}
			} else {
				waiting[parent] = link;
			}
		}
		BOOST_TEST_REQUIRE(next_link == links.size());
	}
	BOOST_TEST(waiting.empty());
	BOOST_TEST(graph.open_lanes() == 0u);
}

}

BOOST_AUTO_TEST_SUITE(EdtCommitGraph)

BOOST_AUTO_TEST_CASE(ALinearHistory_StaysInOneLane)
{
	const std::vector<graph_commit> commits{ { "a", { "b" } }, { "b", { "c" } }, { "c", {} } };
	const std::vector<shape> expected{ { 0, false, { 0 }, k_no_lane, 1 }, { 0, true, { 0 }, k_no_lane, 1 }, { 0, true, {}, k_no_lane, 1 } };
	BOOST_TEST(lay_out(commits) == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(ABranchMergedBack_OpensALaneThatBendsBackIntoTheMainOne)
{
	// m merges f into b; both come from a.
	const std::vector<graph_commit> commits{ { "m", { "b", "f" } }, { "f", { "a" } }, { "b", { "a" } }, { "a", {} } };
	const std::vector<shape> expected{
		{ 0, false, { 0, 1 }, k_no_lane, 2 },
		{ 1, true, { 1 }, k_no_lane, 2 },
		{ 0, true, { 0 }, 1, 2 },
		{ 0, true, {}, k_no_lane, 1 },
	};
	BOOST_TEST(lay_out(commits) == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(AnOctopusMerge_OpensALanePerParent)
{
	const std::vector<graph_commit> commits{
		{ "o", { "a", "b", "c" } }, { "c", { "r" } }, { "b", { "r" } }, { "a", { "r" } }, { "r", {} } };
	const std::vector<shape> expected{
		{ 0, false, { 0, 1, 2 }, k_no_lane, 3 },
		{ 2, true, { 2 }, k_no_lane, 3 },
		{ 1, true, { 1 }, 2, 3 },
		{ 0, true, { 0 }, 1, 2 },
		{ 0, true, {}, k_no_lane, 1 },
	};
	BOOST_TEST(lay_out(commits) == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(ASecondTipOfACommit_JoinsTheLaneThatWaitsForIt)
{
	const std::vector<graph_commit> commits{ { "x", { "p" } }, { "y", { "p" } }, { "p", {} } };
	const std::vector<shape> expected{ { 0, false, { 0 }, k_no_lane, 1 }, { 1, false, { 0 }, k_no_lane, 2 }, { 0, true, {}, k_no_lane, 1 } };
	BOOST_TEST(lay_out(commits) == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(RandomHistories_FollowTheOracle_InEitherOrder)
{
	for (unsigned seed = 1; seed <= 30; ++seed) {
		BOOST_TEST_CONTEXT("seed " << seed) {
			const auto parents = random_history(400, seed);
			for (const auto& order : { natural_order(parents.size()), random_order(parents, seed * 7919) }) {
				const auto commits = commits_of(parents, order);
				commit_graph graph;
				graph.append(commits);
				check_with_oracle(commits, graph);
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(PageByPage_IsTheSameAsAllAtOnce)
{
	for (unsigned seed = 1; seed <= 5; ++seed) {
		const auto parents = random_history(700, seed);
		for (const auto& order : { natural_order(parents.size()), random_order(parents, seed + 100) }) {
			const auto commits = commits_of(parents, order);
			const auto whole = lay_out(commits);
			for (const std::size_t page : { 1u, 7u, 50u, 256u, 699u }) {
				BOOST_TEST_CONTEXT("seed " << seed << ", page " << page) {
					BOOST_TEST((lay_out(commits, page) == whole));
				}
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(NothingAndOneRoot)
{
	commit_graph graph;
	graph.append({});
	BOOST_TEST(graph.rows().empty());
	BOOST_TEST(graph.lanes_above(0).empty());
	const std::vector<graph_commit> root{ { "r", {} } };
	graph.append(root);
	BOOST_TEST((shapes(graph) == std::vector<shape>{ { 0, false, {}, k_no_lane, 1 } }));
	BOOST_TEST(graph.open_lanes() == 0u);
}

BOOST_AUTO_TEST_CASE(AParentThatNeverComes_KeepsItsLaneUntilFinish)
{
	// A shallow or filtered history: "gone" is never listed.
	const std::vector<graph_commit> commits{ { "a", { "b", "gone" } }, { "b", {} } };
	commit_graph graph;
	graph.append(commits);
	BOOST_TEST(graph.open_lanes() == 1u);
	graph.finish();
	BOOST_TEST(graph.open_lanes() == 0u);
	BOOST_TEST(graph.rows().size() == 2u);
}

BOOST_AUTO_TEST_CASE(AParentBeforeItsChild_OpensNoLaneThatWouldHangForever)
{
	const std::vector<graph_commit> commits{ { "p", {} }, { "c", { "p" } } };
	commit_graph graph;
	graph.append(commits);
	BOOST_REQUIRE(graph.rows().size() == 2u);
	BOOST_TEST(graph.rows()[1].dropped == 1u);
	BOOST_TEST(graph.links(graph.rows()[1]).empty());
	BOOST_TEST(graph.open_lanes() == 0u);
}

BOOST_AUTO_TEST_CASE(ACommitTwiceOrAParentTwice_CountsOnce)
{
	const std::vector<graph_commit> commits{ { "a", { "b", "b" } }, { "a", { "x" } }, { "b", {} } };
	commit_graph graph;
	graph.append(commits);
	BOOST_REQUIRE(graph.rows().size() == 2u);
	BOOST_TEST(graph.links(graph.rows()[0]).size() == 1u);
	BOOST_TEST(graph.open_lanes() == 0u);
}

BOOST_AUTO_TEST_CASE(TheLaneCap_DropsTheLinksOverIt)
{
	commit_graph graph;
	graph.set_max_lanes(3);
	const std::vector<graph_commit> commits{ { "o", { "a", "b", "c", "d", "e" } } };
	graph.append(commits);
	BOOST_TEST(graph.open_lanes() == 3u);
	BOOST_TEST(graph.rows()[0].dropped == 2u);
	BOOST_TEST(graph.rows()[0].width <= 3u);
}

BOOST_AUTO_TEST_CASE(ThousandsOfOpenLanes_CostByTheCommits_NotByTheLanes)
{
	// 3000 tips each waiting for its own base, then a long line beside them: every later row passes 3000 lanes.
	std::vector<graph_commit> commits;
	for (std::size_t i = 0; i < 3000; ++i) commits.push_back({ "t" + std::to_string(i), { "base" + std::to_string(i) } });
	for (std::size_t i = 0; i < 20000; ++i) {
		graph_commit line{ "l" + std::to_string(i), {} };
		if (i + 1 < 20000) line.parents.push_back("l" + std::to_string(i + 1));
		commits.push_back(std::move(line));
	}
	commit_graph graph;
	graph.append(commits);
	BOOST_TEST(graph.rows().size() == 23000u);
	BOOST_TEST(graph.open_lanes() == 3000u);
	// A few lane operations per commit and per parent, never one per open lane.
	BOOST_TEST(graph.work() <= 6u * 23000u);
	std::size_t links = 0;
	for (const auto& row : graph.rows()) links += row.links_count;
	BOOST_TEST(links <= 23000u);
	// The renderer still sees every lane that passes a row.
	BOOST_TEST(graph.lanes_above(22999).size() == 3001u);
	graph.finish();
	BOOST_TEST(graph.open_lanes() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
