#include <boost/test/unit_test.hpp>
#include "edt_commit_graph.h"
#include <algorithm>
#include <cstddef>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

// The lanes of the Git panel's commit graph (#457): hand-checked histories, and properties on random ones laid out in
// two different child-before-parent orders (date order and topological order both are such orders).
namespace {

using edt::vcs::commit_graph;
using edt::vcs::graph_commit;
using edt::vcs::graph_row;
using edt::vcs::lane_link;

graph_row row(std::size_t lane, std::vector<lane_link> up, std::vector<lane_link> down, std::size_t width)
{
	return graph_row{ lane, std::move(up), std::move(down), width };
}

std::vector<graph_row> lay_out(const std::vector<graph_commit>& commits, std::size_t page = 0)
{
	commit_graph graph;
	if (page == 0) {
		graph.append(commits);
	} else {
		for (std::size_t at = 0; at < commits.size(); at += page) {
			graph.append(std::span(commits).subspan(at, std::min(page, commits.size() - at)));
		}
	}
	return graph.rows();
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

void check_lines(const std::vector<graph_row>& rows, const commit_graph& graph)
{
	for (std::size_t r = 0; r < rows.size(); ++r) {
		const graph_row& at = rows[r];
		for (const auto& [from, to] : at.up) {
			BOOST_TEST_REQUIRE(from < at.width);
			BOOST_TEST_REQUIRE(to < at.width);
			// A lane either passes or comes into the commit.
			BOOST_TEST_REQUIRE((from == to || to == at.lane));
		}
		for (const auto& [from, to] : at.down) {
			BOOST_TEST_REQUIRE(from < at.width);
			BOOST_TEST_REQUIRE(to < at.width);
		}
		// Every line that leaves a row goes on in the next one, and nothing comes from nowhere.
		if (r + 1 < rows.size()) {
			std::set<std::size_t> leaving;
			for (const auto& link : at.down) leaving.insert(link.second);
			std::set<std::size_t> arriving;
			for (const auto& link : rows[r + 1].up) arriving.insert(link.first);
			BOOST_TEST_REQUIRE((leaving == arriving));
		}
	}
	BOOST_TEST(graph.open_lanes() == 0u);
}

}

BOOST_AUTO_TEST_SUITE(EdtCommitGraph)

BOOST_AUTO_TEST_CASE(ALinearHistory_StaysInOneLane)
{
	const std::vector<graph_commit> commits{ { "a", { "b" } }, { "b", { "c" } }, { "c", {} } };
	const std::vector<graph_row> expected{
		row(0, {}, { { 0, 0 } }, 1),
		row(0, { { 0, 0 } }, { { 0, 0 } }, 1),
		row(0, { { 0, 0 } }, {}, 1),
	};
	BOOST_TEST((lay_out(commits) == expected));
}

BOOST_AUTO_TEST_CASE(ABranchMergedBack_OpensALaneAndBendsBackIntoTheMainOne)
{
	// m merges f into b; both come from a.
	const std::vector<graph_commit> commits{ { "m", { "b", "f" } }, { "f", { "a" } }, { "b", { "a" } }, { "a", {} } };
	const std::vector<graph_row> expected{
		row(0, {}, { { 0, 0 }, { 0, 1 } }, 2),
		row(1, { { 0, 0 }, { 1, 1 } }, { { 0, 0 }, { 1, 1 } }, 2),
		row(0, { { 0, 0 }, { 1, 1 } }, { { 0, 0 }, { 1, 0 } }, 2),
		row(0, { { 0, 0 } }, {}, 1),
	};
	BOOST_TEST((lay_out(commits) == expected));
}

BOOST_AUTO_TEST_CASE(AnOctopusMerge_OpensALanePerParent)
{
	const std::vector<graph_commit> commits{
		{ "o", { "a", "b", "c" } }, { "c", { "r" } }, { "b", { "r" } }, { "a", { "r" } }, { "r", {} } };
	const std::vector<graph_row> expected{
		row(0, {}, { { 0, 0 }, { 0, 1 }, { 0, 2 } }, 3),
		row(2, { { 0, 0 }, { 1, 1 }, { 2, 2 } }, { { 0, 0 }, { 1, 1 }, { 2, 2 } }, 3),
		row(1, { { 0, 0 }, { 1, 1 }, { 2, 2 } }, { { 0, 0 }, { 1, 1 }, { 2, 1 } }, 3),
		row(0, { { 0, 0 }, { 1, 1 } }, { { 0, 0 }, { 1, 0 } }, 2),
		row(0, { { 0, 0 } }, {}, 1),
	};
	BOOST_TEST((lay_out(commits) == expected));
}

BOOST_AUTO_TEST_CASE(ASecondTipOfACommit_BendsIntoTheLaneThatWaitsForIt)
{
	// x and y both have p as parent: y opens a lane and goes straight into x's, which carries on to p.
	const std::vector<graph_commit> commits{ { "x", { "p" } }, { "y", { "p" } }, { "p", {} } };
	const std::vector<graph_row> expected{
		row(0, {}, { { 0, 0 } }, 1),
		row(1, { { 0, 0 } }, { { 0, 0 }, { 1, 0 } }, 2),
		row(0, { { 0, 0 } }, {}, 1),
	};
	BOOST_TEST((lay_out(commits) == expected));
}

BOOST_AUTO_TEST_CASE(RandomHistories_KeepTheirLinesWhole_InEitherOrder)
{
	for (unsigned seed = 1; seed <= 40; ++seed) {
		BOOST_TEST_CONTEXT("seed " << seed) {
			const auto parents = random_history(300, seed);
			std::vector<std::size_t> natural(parents.size());
			for (std::size_t i = 0; i < natural.size(); ++i) natural[i] = i;
			for (const auto& order : { natural, random_order(parents, seed * 7919) }) {
				const auto commits = commits_of(parents, order);
				commit_graph graph;
				graph.append(commits);
				check_lines(graph.rows(), graph);
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(PageByPage_IsTheSameAsAllAtOnce)
{
	const auto parents = random_history(500, 42);
	std::vector<std::size_t> order(parents.size());
	for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
	const auto commits = commits_of(parents, order);
	const auto whole = lay_out(commits);
	for (const std::size_t page : { 1u, 7u, 50u, 200u, 499u }) {
		BOOST_TEST_CONTEXT("page " << page) {
			BOOST_TEST((lay_out(commits, page) == whole));
		}
	}
}

BOOST_AUTO_TEST_CASE(AHundredThousandCommits_LayOutInOnePass)
{
	std::vector<graph_commit> commits;
	commits.reserve(100000);
	for (std::size_t i = 0; i < 100000; ++i) {
		graph_commit commit{ std::to_string(i), {} };
		if (i + 1 < 100000) commit.parents.push_back(std::to_string(i + 1));
		if (i % 50 == 0 && i + 5 < 100000) commit.parents.push_back(std::to_string(i + 5));
		commits.push_back(std::move(commit));
	}
	commit_graph graph;
	graph.append(commits);
	BOOST_TEST(graph.rows().size() == 100000u);
	BOOST_TEST(graph.open_lanes() == 0u);
	std::size_t widest = 0;
	for (const auto& r : graph.rows()) widest = std::max(widest, r.width);
	BOOST_TEST(widest <= 3u);
}

BOOST_AUTO_TEST_SUITE_END()
