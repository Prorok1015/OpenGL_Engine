#include <boost/test/unit_test.hpp>
#include "edt_history_model.h"
#include <snk/editor/edt_editor_events.h>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The History tab's model (#457): which vcs.log pages it asks for as rows come into view, what a filter, a stale
// cursor and a selection do.
namespace {

namespace json = boost::json;
using namespace std::chrono_literals;
using edt::vcs::history_model;

std::string hash_of(std::size_t index)
{
	return std::format("{:040x}", index + 1);
}

struct history_fixture
{
	struct call
	{
		std::string id;
		json::object args;
		std::uint64_t job = 0;
	};

	edt::editor_events events;
	std::vector<call> calls;
	std::vector<std::uint64_t> cancelled;
	std::uint64_t next_job = 1;
	history_model::clock::time_point now{};
	// Newest first; commit i's first parent is i + 1, and every tenth one also merges i + 2.
	std::size_t history_size = 1000;
	// A command invoke refuses, as a registry would refuse arguments it does not know.
	std::string refused;
	history_model model{ events, ports() };

	history_model::ports ports(bool extended_reads = true)
	{
		history_model::ports out;
		out.extended_reads = extended_reads;
		out.invoke = [this](std::string_view id, json::value args) {
			if (id == refused) return cmd::result::failure("invalid_args", "unknown argument 'all'");
			const std::uint64_t job = next_job++;
			calls.push_back({ std::string(id), args.as_object(), job });
			return cmd::result::success(json::object{ { "job", job } });
		};
		out.cancel = [this](std::uint64_t job) {
			cancelled.push_back(job);
			return true;
		};
		out.now = [this] { return now; };
		return out;
	}

	std::size_t count(std::string_view id) const
	{
		std::size_t n = 0;
		for (const auto& c : calls) n += c.id == id ? 1 : 0;
		return n;
	}

	std::optional<call> last(std::string_view id) const
	{
		for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
			if (it->id == id) return *it;
		}
		return std::nullopt;
	}

	void finish(const call& started, json::value result, edt::job_status status = edt::job_status::done)
	{
		events.job_finished.emit(started.job, status, std::string{}, std::optional<json::value>{ std::move(result) });
	}

	json::object commit_json(std::size_t index) const
	{
		json::array parents;
		if (index + 1 < history_size) parents.emplace_back(hash_of(index + 1));
		if (index % 10 == 0 && index + 2 < history_size) parents.emplace_back(hash_of(index + 2));
		return json::object{ { "hash", hash_of(index) }, { "parents", std::move(parents) }, { "author", "Ann" },
			{ "email", "ann@example.com" }, { "time", 1700000000 - static_cast<std::int64_t>(index) },
			{ "subject", std::format("commit {}", index) } };
	}

	// Answers the last vcs.log call from the synthetic history; the cursor is "c<skip>".
	void answer_log()
	{
		const auto started = last("vcs.log");
		BOOST_REQUIRE(started);
		std::size_t skip = 0;
		if (const json::value* cursor = started->args.if_contains("cursor")) {
			skip = std::stoul(std::string(cursor->as_string()).substr(1));
		}
		const auto limit = static_cast<std::size_t>(started->args.at("limit").to_number<std::int64_t>());
		json::array commits;
		const std::size_t end = std::min(history_size, skip + limit);
		for (std::size_t i = skip; i < end; ++i) commits.emplace_back(commit_json(i));
		json::object result{ { "commits", std::move(commits) } };
		if (end < history_size) result["next_cursor"] = std::format("c{}", end);
		finish(*started, std::move(result));
	}

	void open_and_load()
	{
		model.set_visible(true);
		answer_log();
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtHistoryModel, history_fixture)

BOOST_AUTO_TEST_CASE(AHiddenTab_AsksForNothing_UntilItIsShown)
{
	model.set_visible(false);
	model.show_rows(0, 40);
	BOOST_TEST(calls.empty());

	model.set_visible(true);
	BOOST_REQUIRE_EQUAL(calls.size(), 1u);
	const call& page = calls.front();
	BOOST_TEST(page.id == "vcs.log");
	BOOST_TEST(page.args.at("all").as_bool());
	BOOST_TEST(page.args.at("limit").to_number<int>() == history_model::k_page_size);
	BOOST_TEST(!page.args.contains("cursor"));
	BOOST_TEST(!page.args.contains("ref"));
	BOOST_TEST(model.loading());
}

BOOST_AUTO_TEST_CASE(ShownAgainAfterBeingHidden_ReloadsFromTheTop)
{
	open_and_load();
	model.show_rows(150, 190);
	answer_log();
	BOOST_REQUIRE_EQUAL(model.commits().size(), 400u);

	model.set_visible(false);
	model.set_visible(true);
	BOOST_TEST(count("vcs.log") == 3u);
	BOOST_TEST(!last("vcs.log")->args.contains("cursor"));
	BOOST_TEST(model.commits().empty());
	model.set_visible(true);
	BOOST_TEST(count("vcs.log") == 3u);
}

BOOST_AUTO_TEST_CASE(APage_BecomesRows_AndTheirGraph)
{
	open_and_load();
	BOOST_TEST(!model.loading());
	BOOST_REQUIRE_EQUAL(model.commits().size(), static_cast<std::size_t>(history_model::k_page_size));
	BOOST_TEST(model.graph().rows().size() == model.commits().size());
	BOOST_TEST(model.commits()[3].subject == "commit 3");
	BOOST_TEST(model.commits()[10].parents.size() == 2u);
	BOOST_TEST(!model.at_end());
}

BOOST_AUTO_TEST_CASE(TheFirstScreens_AskForOnePage)
{
	open_and_load();
	model.show_rows(0, 40);
	model.show_rows(40, 80);
	BOOST_TEST(count("vcs.log") == 1u);
}

BOOST_AUTO_TEST_CASE(ScrollingToTheEndOfAHundredThousandCommits_AsksForEachPageOnce)
{
	history_size = 100000;
	open_and_load();
	constexpr std::size_t screen = 40;
	for (std::size_t first = 0; !model.at_end(); first += screen) {
		model.show_rows(first, first + screen);
		if (model.loading()) answer_log();
		BOOST_REQUIRE(first < history_size);
	}
	BOOST_TEST(count("vcs.log") == (history_size + history_model::k_page_size - 1) / history_model::k_page_size);
	BOOST_TEST(model.commits().size() == history_size);
	BOOST_TEST(model.graph().rows().size() == history_size);
	BOOST_TEST(model.graph().open_lanes() == 0u);

	model.show_rows(history_size - screen, history_size);
	BOOST_TEST(!model.loading());
}

BOOST_AUTO_TEST_CASE(TheNextPage_AsksWithTheCursorOfTheLastOne)
{
	open_and_load();
	model.show_rows(150, 190);
	const auto page = last("vcs.log");
	BOOST_REQUIRE(page);
	BOOST_TEST(page->args.at("cursor").as_string() == "c200");
}

BOOST_AUTO_TEST_CASE(ATallScreen_KeepsLoadingUntilItIsFilled)
{
	open_and_load();
	model.show_rows(0, 500);
	answer_log();
	BOOST_TEST(model.loading());
	answer_log();
	BOOST_TEST(model.commits().size() == 600u);
	BOOST_TEST(!model.loading());
}

BOOST_AUTO_TEST_CASE(AFilter_ReloadsAfterTheQuietTime_WithItsArguments)
{
	open_and_load();
	model.show_rows(150, 190);
	const std::uint64_t running = last("vcs.log")->job;

	model.set_filter({ "main", "ann", "fix" });
	model.tick();
	now += history_model::k_filter_delay - 1ms;
	model.tick();
	BOOST_TEST(count("vcs.log") == 2u);

	now += 1ms;
	model.tick();
	BOOST_REQUIRE_EQUAL(count("vcs.log"), 3u);
	BOOST_REQUIRE_EQUAL(cancelled.size(), 1u);
	BOOST_TEST(cancelled.front() == running);
	const auto page = last("vcs.log");
	BOOST_TEST(page->args.at("ref").as_string() == "main");
	BOOST_TEST(page->args.at("author").as_string() == "ann");
	BOOST_TEST(page->args.at("grep").as_string() == "fix");
	BOOST_TEST(!page->args.contains("all"));
	BOOST_TEST(!page->args.contains("cursor"));
	BOOST_TEST(model.commits().empty());
}

BOOST_AUTO_TEST_CASE(TheSameFilterAgain_ChangesNothing)
{
	open_and_load();
	model.set_filter({});
	now += 1s;
	model.tick();
	BOOST_TEST(count("vcs.log") == 1u);
}

BOOST_AUTO_TEST_CASE(APageOfTheOldFilter_ArrivingLate_IsDropped)
{
	open_and_load();
	model.show_rows(150, 190);
	const call old_page = *last("vcs.log");
	model.set_filter({ "main", "", "" });
	now += history_model::k_filter_delay;
	model.tick();

	finish(old_page, json::object{ { "commits", json::array{ commit_json(500) } } });
	BOOST_TEST(model.commits().empty());
	BOOST_TEST(model.loading());
}

BOOST_AUTO_TEST_CASE(AStaleCursor_StartsAgainFromTheTop)
{
	open_and_load();
	model.show_rows(150, 190);
	finish(*last("vcs.log"), json::parse(R"({"error":{"code":"stale","message":"the history changed"}})"));

	BOOST_REQUIRE_EQUAL(count("vcs.log"), 3u);
	BOOST_TEST(!last("vcs.log")->args.contains("cursor"));
	BOOST_TEST(model.commits().empty());
	BOOST_TEST(model.error().empty());
	answer_log();
	BOOST_TEST(model.commits().size() == 200u);
}

BOOST_AUTO_TEST_CASE(ACursorThatKeepsGoingStale_GivesUp_UntilARefresh)
{
	model.set_visible(true);
	for (int i = 0; i <= history_model::k_max_restarts; ++i) {
		finish(*last("vcs.log"), json::parse(R"({"error":{"code":"stale"}})"));
	}
	const std::size_t asked = count("vcs.log");
	BOOST_TEST(asked == static_cast<std::size_t>(history_model::k_max_restarts) + 1);
	BOOST_TEST(!model.loading());
	BOOST_TEST(!model.error().empty());
	model.show_rows(0, 40);
	BOOST_TEST(count("vcs.log") == asked);

	model.reload();
	BOOST_TEST(count("vcs.log") == asked + 1);
	BOOST_TEST(model.error().empty());
}

BOOST_AUTO_TEST_CASE(AGitError_ShowsAndStopsThePaging)
{
	open_and_load();
	model.show_rows(150, 190);
	finish(*last("vcs.log"), json::parse(R"({"error":{"code":"bad_revision","message":"fatal: bad revision 'nope'"}})"));
	BOOST_TEST(model.error() == "History: fatal: bad revision 'nope'");
	model.show_rows(160, 199);
	BOOST_TEST(count("vcs.log") == 2u);
	BOOST_TEST(model.commits().size() == 200u);
}

BOOST_AUTO_TEST_CASE(SelectingACommit_ReadsItsFiles_ThenAFilesDiff)
{
	open_and_load();
	model.select(4);
	const auto files = last("vcs.show_files");
	BOOST_REQUIRE(files);
	BOOST_TEST(files->args.at("commit").as_string() == hash_of(4));
	BOOST_TEST(model.files_loading());
	finish(*files, json::parse(R"({"files":[{"path":"src/a.cpp","status":"M"},{"path":"src/new.cpp","status":"R","orig_path":"src/old.cpp"}]})"));
	BOOST_REQUIRE_EQUAL(model.files().size(), 2u);
	BOOST_TEST(model.files()[1].status == 'R');
	BOOST_TEST(model.files()[1].orig_path == "src/old.cpp");

	model.select_file("src/a.cpp");
	const auto diff = last("vcs.diff");
	BOOST_REQUIRE(diff);
	BOOST_TEST(diff->args.at("commit").as_string() == hash_of(4));
	BOOST_TEST(diff->args.at("path").as_string() == "src/a.cpp");
	finish(*diff, json::parse(R"({"text":"@@ -1 +1 @@\n-a\n+b\n","truncated":true})"));
	BOOST_TEST(model.diff_text() == "@@ -1 +1 @@\n-a\n+b\n");
	BOOST_TEST(model.diff_truncated());
	BOOST_TEST((model.selected_row() == std::optional<std::size_t>{ 4 }));
}

BOOST_AUTO_TEST_CASE(TheDiffVersion_ChangesWhenTheDiffTextDoes)
{
	open_and_load();
	model.select(4);
	finish(*last("vcs.show_files"), json::parse(R"({"files":[{"path":"a.cpp","status":"M"}]})"));
	const std::uint64_t before = model.diff_version();
	model.select_file("a.cpp");
	const std::uint64_t cleared = model.diff_version();
	BOOST_TEST(cleared != before);
	finish(*last("vcs.diff"), json::parse(R"({"text":"+x\n"})"));
	const std::uint64_t read = model.diff_version();
	BOOST_TEST(read != cleared);
	model.select(5);
	BOOST_TEST(model.diff_version() != read);
	BOOST_TEST(model.diff_text().empty());
}

BOOST_AUTO_TEST_CASE(AnotherCommit_DropsTheFilesOfTheFirst)
{
	open_and_load();
	model.select(4);
	const call first = *last("vcs.show_files");
	model.select(5);
	BOOST_REQUIRE_EQUAL(cancelled.size(), 1u);
	BOOST_TEST(cancelled.front() == first.job);
	finish(first, json::parse(R"({"files":[{"path":"late.cpp","status":"M"}]})"));
	BOOST_TEST(model.files().empty());
	BOOST_TEST(model.files_loading());
	BOOST_TEST(model.selected_commit() == hash_of(5));
}

BOOST_AUTO_TEST_CASE(TheSelection_FindsItsRowAgainAfterARefresh)
{
	open_and_load();
	model.select(5);
	finish(*last("vcs.show_files"), json::parse(R"({"files":[{"path":"a.cpp","status":"A"}]})"));
	model.reload();
	BOOST_TEST(!model.selected_row());
	BOOST_TEST(model.files().size() == 1u);
	answer_log();
	BOOST_TEST((model.selected_row() == std::optional<std::size_t>{ 5 }));
}

BOOST_AUTO_TEST_CASE(TheLastPage_EndsTheHistory)
{
	history_size = 150;
	open_and_load();
	BOOST_TEST(model.at_end());
	BOOST_TEST(model.commits().size() == 150u);
	model.show_rows(110, 149);
	BOOST_TEST(count("vcs.log") == 1u);
}

BOOST_AUTO_TEST_CASE(ACommitOfAPage_IsReadWithItsRefs)
{
	const auto commit = edt::vcs::history_commit_of(json::parse(R"({"hash":"abc","parents":["p1","p2"],"author":"Ann",
		"email":"a@x","time":12,"subject":"s","refs":[{"name":"HEAD","kind":"head"},{"name":"main","kind":"branch"},
		{"name":"origin/main","kind":"remote"},{"name":"v1","kind":"tag"},{"name":"odd","kind":"stash"},{"kind":"tag"}]})").as_object());
	BOOST_REQUIRE(commit);
	BOOST_TEST(commit->parents.size() == 2u);
	BOOST_TEST(commit->time == 12);
	BOOST_REQUIRE_EQUAL(commit->refs.size(), 4u);
	BOOST_TEST((commit->refs[0].type == edt::vcs::history_ref::kind::head));
	BOOST_TEST((commit->refs[2].type == edt::vcs::history_ref::kind::remote));
	BOOST_TEST(commit->refs[3].name == "v1");

	BOOST_TEST(!edt::vcs::history_commit_of(json::parse(R"({"subject":"no hash"})").as_object()));
}

BOOST_AUTO_TEST_CASE(ANewFilterDuringAPage_LeavesThatPagesPagingAsItWas)
{
	open_and_load();
	model.show_rows(150, 190);
	model.set_filter({ "main", "", "" });
	answer_log();
	BOOST_TEST(model.commits().size() == 400u);
	model.show_rows(350, 390);
	const auto page = last("vcs.log");
	BOOST_TEST(page->args.at("all").as_bool());
	BOOST_TEST(!page->args.contains("ref"));
	BOOST_TEST(page->args.at("cursor").as_string() == "c400");

	now += history_model::k_filter_delay;
	model.tick();
	BOOST_TEST(last("vcs.log")->args.at("ref").as_string() == "main");
	BOOST_TEST(!last("vcs.log")->args.contains("cursor"));
}

BOOST_AUTO_TEST_CASE(ANewFilter_DropsTheSelection_ARefreshKeepsIt)
{
	open_and_load();
	model.select(5);
	model.reload();
	BOOST_TEST(model.selected_commit() == hash_of(5));
	model.set_filter({ "", "", "fix" });
	now += history_model::k_filter_delay;
	model.tick();
	BOOST_TEST(model.selected_commit().empty());
	BOOST_TEST(!model.files_loading());
}

BOOST_AUTO_TEST_CASE(AFilterChangeDuringAStaleRestart_AsksWithTheNewFilterFromTheTop)
{
	open_and_load();
	model.show_rows(150, 190);
	finish(*last("vcs.log"), json::parse(R"({"error":{"code":"stale"}})"));
	const call restarted = *last("vcs.log");
	model.set_filter({ "dev", "", "" });
	now += history_model::k_filter_delay;
	model.tick();
	BOOST_TEST(!cancelled.empty());
	BOOST_TEST(cancelled.back() == restarted.job);
	finish(restarted, json::object{ { "commits", json::array{ commit_json(0) } }, { "next_cursor", "c1" } });
	BOOST_TEST(model.commits().empty());
	BOOST_TEST(last("vcs.log")->args.at("ref").as_string() == "dev");
}

BOOST_AUTO_TEST_CASE(APageThatBringsNothingNew_EndsTheHistory)
{
	open_and_load();
	model.show_rows(150, 190);
	json::array repeated;
	repeated.emplace_back(commit_json(3));
	finish(*last("vcs.log"), json::object{ { "commits", std::move(repeated) }, { "next_cursor", "c400" } });
	BOOST_TEST(model.at_end());
	model.show_rows(160, 199);
	BOOST_TEST(count("vcs.log") == 2u);
}

BOOST_AUTO_TEST_CASE(ARefusedLogCall_ShowsWhy_AndIsNotRepeatedOnScroll)
{
	refused = "vcs.log";
	model.set_visible(true);
	BOOST_TEST(calls.empty());
	BOOST_TEST(!model.loading());
	BOOST_TEST(model.error() == "vcs.log: unknown argument 'all'");
	model.show_rows(0, 40);
	BOOST_TEST(calls.empty());
}

BOOST_AUTO_TEST_CASE(AModelDestroyedWithAJobRunning_CancelsIt_AndIgnoresItsEnd)
{
	auto owned = std::make_unique<history_model>(events, ports());
	owned->set_visible(true);
	const call page = *last("vcs.log");
	owned.reset();
	BOOST_REQUIRE_EQUAL(cancelled.size(), 1u);
	BOOST_TEST(cancelled.front() == page.job);
	finish(page, json::object{ { "commits", json::array{ commit_json(0) } } });
}

BOOST_AUTO_TEST_CASE(WithoutTheExtendedReads_ItShowsHeadsHistory_AndNoFiles)
{
	history_model plain{ events, ports(false) };
	plain.set_visible(true);
	const call page = *last("vcs.log");
	BOOST_TEST(!page.args.contains("all"));
	plain.set_filter({ "main", "ann", "fix" });
	now += history_model::k_filter_delay;
	plain.tick();
	const auto filtered = last("vcs.log");
	BOOST_TEST(filtered->args.at("ref").as_string() == "main");
	BOOST_TEST(!filtered->args.contains("author"));
	BOOST_TEST(!filtered->args.contains("grep"));
	BOOST_TEST(!plain.filters_by_text());

	answer_log();
	plain.select(2);
	plain.select_file("a.cpp");
	BOOST_TEST(plain.selected_commit() == hash_of(2));
	BOOST_TEST(count("vcs.show_files") == 0u);
	BOOST_TEST(count("vcs.diff") == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
