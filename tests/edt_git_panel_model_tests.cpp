#include <boost/test/unit_test.hpp>
#include "edt_git_panel_model.h"
#include <snk/editor/edt_editor_events.h>
#include <boost/json.hpp>
#include <chrono>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The Git panel's model (#456): what it asks the vcs.* commands for and what it makes of their jobs' results.
namespace {

namespace json = boost::json;
using namespace std::chrono_literals;

struct panel_fixture
{
	struct call
	{
		std::string id;
		json::value args;
		std::uint64_t job = 0;
	};

	edt::editor_events events;
	std::vector<call> calls;
	std::vector<std::uint64_t> cancelled;
	std::uint64_t next_job = 1;
	json::value info = json::object{ { "available", true } };
	edt::git_panel_model::clock::time_point now{};
	edt::git_panel_model model{ events, ports() };

	edt::git_panel_model::ports ports()
	{
		edt::git_panel_model::ports out;
		out.invoke = [this](std::string_view id, json::value args) {
			if (id == "vcs.info") {
				calls.push_back({ std::string(id), std::move(args), 0 });
				return cmd::result::success(info);
			}
			const std::uint64_t job = next_job++;
			calls.push_back({ std::string(id), std::move(args), job });
			return cmd::result::success(json::object{ { "job", job } });
		};
		out.cancel = [this](std::uint64_t job) {
			cancelled.push_back(job);
			return true;
		};
		out.now = [this] { return now; };
		return out;
	}

	std::optional<call> last(std::string_view id) const
	{
		for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
			if (it->id == id) return *it;
		}
		return std::nullopt;
	}

	void finish(std::string_view id, json::value result, edt::job_status status = edt::job_status::done)
	{
		const auto started = last(id);
		BOOST_REQUIRE_MESSAGE(started, std::string(id) + " was not started");
		events.job_finished.emit(started->job, status, std::string{}, std::optional<json::value>{ std::move(result) });
	}

	static json::value status_result()
	{
		return json::parse(R"({"branch":{"head":"main","detached":false,"oid":"abc","upstream":"origin/main","ahead":2,"behind":1},
			"entries":[{"path":"src/a.cpp","index":"M","worktree":".","kind":"ordinary"},
				{"path":"src/b.cpp","index":".","worktree":"M","kind":"ordinary"},
				{"path":"src/both.cpp","index":"M","worktree":"M","kind":"ordinary"},
				{"path":"new.cpp","index":"?","worktree":"?","kind":"untracked"}],"clean":false,"truncated":false})");
	}

	void open_and_read()
	{
		model.set_visible(true);
		finish("vcs.status", status_result());
		finish("vcs.branches", json::parse(R"({"current":"main","branches":[{"name":"main","full_name":"refs/heads/main","oid":"abc","head":true,"remote":false},
			{"name":"origin/feature","full_name":"refs/remotes/origin/feature","oid":"def","head":false,"remote":true}],"truncated":false})"));
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtGitPanelModel, panel_fixture)

BOOST_AUTO_TEST_CASE(AClosedPanel_AsksGitForNothing)
{
	model.set_visible(false);
	model.files_changed();
	now += 5s;
	model.tick();
	BOOST_TEST(calls.empty());
}

BOOST_AUTO_TEST_CASE(Opening_ReadsStatusAndBranches_AndSortsTheChanges)
{
	open_and_read();
	BOOST_TEST(model.available());
	BOOST_TEST(model.head() == "main");
	BOOST_TEST(model.upstream() == "origin/main");
	BOOST_TEST(model.ahead().value_or(-1) == 2);
	BOOST_TEST(model.behind().value_or(-1) == 1);
	BOOST_REQUIRE(model.staged().size() == 2u);
	BOOST_TEST(model.staged()[0].path == "src/a.cpp");
	BOOST_REQUIRE(model.unstaged().size() == 2u);
	BOOST_TEST(model.unstaged()[0].path == "src/b.cpp");
	BOOST_REQUIRE(model.untracked().size() == 1u);
	BOOST_TEST(model.untracked()[0].path == "new.cpp");
	BOOST_REQUIRE(model.branches().size() == 2u);
	BOOST_TEST(model.branches()[1].remote);
	for (const auto& c : calls) BOOST_TEST(c.id.starts_with("vcs."));
}

BOOST_AUTO_TEST_CASE(WithoutARepository_ItSaysWhy_AndStartsNoJob)
{
	info = json::object{ { "available", false }, { "reason", "not a git repository" } };
	model.set_visible(true);
	BOOST_TEST(!model.available());
	BOOST_TEST(model.unavailable_reason() == "not a git repository");
	BOOST_TEST(!last("vcs.status"));
	BOOST_TEST(model.error().empty());
}

BOOST_AUTO_TEST_CASE(ACodeChange_RefreshesOnceAfterTheQuietTime_WhileVisible)
{
	open_and_read();
	const auto before = calls.size();
	model.files_changed();
	now += 500ms;
	model.files_changed();
	model.tick();
	BOOST_TEST(calls.size() == before);
	const auto status_before = last("vcs.status")->job;
	now += 1001ms;
	model.tick();
	model.tick();
	BOOST_TEST(last("vcs.status")->job != status_before);
	BOOST_TEST(calls.size() == before + 3u); // info, status, branches
}

BOOST_AUTO_TEST_CASE(AChangeWhileHidden_RefreshesWhenShownAgain)
{
	open_and_read();
	model.set_visible(false);
	const auto before = calls.size();
	model.files_changed();
	BOOST_TEST(calls.size() == before);
	model.set_visible(true);
	BOOST_TEST(calls.size() == before + 3u);
}

BOOST_AUTO_TEST_CASE(StagingAFile_RunsTheCommand_ThenReadsAgain)
{
	open_and_read();
	model.set_staged("src/b.cpp", true);
	const auto staged = last("vcs.stage");
	BOOST_REQUIRE(staged);
	BOOST_TEST(json::serialize(staged->args) == R"({"paths":["src/b.cpp"]})");
	BOOST_TEST(model.busy() == "Stage");
	const auto status_before = last("vcs.status")->job;
	finish("vcs.stage", json::object{ { "output", "" } });
	BOOST_TEST(model.busy().empty());
	BOOST_TEST(last("vcs.status")->job > status_before);

	model.set_staged("src/a.cpp", false);
	BOOST_TEST(json::serialize(last("vcs.unstage")->args) == R"({"paths":["src/a.cpp"]})");
}

BOOST_AUTO_TEST_CASE(OneWriteAtATime)
{
	open_and_read();
	model.fetch();
	model.pull();
	BOOST_TEST(!last("vcs.pull"));
	BOOST_TEST(model.error().find("Fetch") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(EveryEndedWrite_IsCounted_AReadIsNot)
{
	open_and_read();
	BOOST_TEST(model.writes_finished() == 0u);
	model.fetch();
	finish("vcs.fetch", json::object{ { "output", "" } });
	BOOST_TEST(model.writes_finished() == 1u);
	model.pull();
	finish("vcs.pull", json::object{}, edt::job_status::cancelled);
	BOOST_TEST(model.writes_finished() == 2u);
	finish("vcs.status", status_result());
	BOOST_TEST(model.writes_finished() == 2u);
}

BOOST_AUTO_TEST_CASE(AGitError_ShowsInThePanel)
{
	open_and_read();
	model.pull();
	finish("vcs.pull", json::parse(R"({"error":{"code":"network","message":"could not resolve host","stderr_tail":"","exit_code":128}})"));
	BOOST_TEST(model.error() == "Pull: could not resolve host");
	BOOST_TEST(model.busy().empty());
	model.clear_error();
	BOOST_TEST(model.error().empty());
}

BOOST_AUTO_TEST_CASE(Cancel_StopsTheRunningWrite)
{
	open_and_read();
	model.push();
	const auto push = last("vcs.push");
	BOOST_REQUIRE(push);
	BOOST_TEST(json::serialize(push->args) == R"({"branch":"main","remote":"origin"})");
	model.cancel();
	BOOST_REQUIRE(cancelled.size() == 1u);
	BOOST_TEST(cancelled[0] == push->job);
	finish("vcs.push", json::value{}, edt::job_status::cancelled);
	BOOST_TEST(model.busy().empty());
	BOOST_TEST(model.error().empty());
}

BOOST_AUTO_TEST_CASE(TheRunningWritesProgress_IsShown_UntilItEnds)
{
	open_and_read();
	model.fetch();
	const auto fetch = last("vcs.fetch");
	BOOST_REQUIRE(fetch);
	BOOST_TEST(!model.busy_progress().has_value());

	events.job_progress.emit(fetch->job + 100, 0.7f, "Writing objects 70%");
	BOOST_TEST(!model.busy_progress().has_value());

	events.job_progress.emit(fetch->job, 0.46f, "Receiving objects 45%");
	BOOST_REQUIRE(model.busy_progress().has_value());
	BOOST_TEST(model.busy_progress()->fraction == 0.46f);
	BOOST_TEST(model.busy_progress()->text == "Receiving objects 45%");

	events.job_progress.emit(fetch->job + 100, 0.9f, "Resolving deltas 100%");
	BOOST_TEST(model.busy_progress()->text == "Receiving objects 45%");

	finish("vcs.fetch", json::object{ { "output", "" } });
	BOOST_TEST(model.busy().empty());
	BOOST_TEST(!model.busy_progress().has_value());

	// A late update of a write that has ended belongs to no running write.
	events.job_progress.emit(fetch->job, 1.0f, "Resolving deltas 100%");
	BOOST_TEST(!model.busy_progress().has_value());
}

BOOST_AUTO_TEST_CASE(PushWithoutAnUpstream_SetsOne)
{
	model.set_visible(true);
	finish("vcs.status", json::parse(R"({"branch":{"head":"topic","detached":false,"oid":"abc"},"entries":[],"clean":true,"truncated":false})"));
	model.push();
	BOOST_TEST(json::serialize(last("vcs.push")->args) == R"({"branch":"topic","remote":"origin","set_upstream":true})");
}

BOOST_AUTO_TEST_CASE(ARemoteBranch_IsCheckedOutByItsName)
{
	open_and_read();
	model.checkout(model.branches()[1]);
	BOOST_TEST(json::serialize(last("vcs.checkout")->args) == R"({"ref":"feature"})");
}

BOOST_AUTO_TEST_CASE(Commit_SendsTheMessage_AndClearsItOnlyOnceItSucceeded)
{
	open_and_read();
	model.message() = "   \n";
	BOOST_TEST(!model.can_commit());
	model.message() = "Fix the thing";
	BOOST_TEST(model.can_commit());
	model.commit();
	BOOST_TEST(json::serialize(last("vcs.commit")->args) == R"({"message":"Fix the thing"})");
	finish("vcs.commit", json::parse(R"({"error":{"code":"git_failed","message":"pre-commit hook refused","stderr_tail":"","exit_code":1}})"));
	BOOST_TEST(model.message() == "Fix the thing");

	finish("vcs.status", status_result());
	model.commit();
	finish("vcs.commit", json::object{ { "hash", "abc" } });
	BOOST_TEST(model.message().empty());
}

BOOST_AUTO_TEST_CASE(AfterStaging_TheSelectionFollowsTheFile)
{
	open_and_read();
	model.select("src/b.cpp", false);
	model.set_staged("src/b.cpp", true);
	finish("vcs.stage", json::object{ { "output", "" } });
	finish("vcs.status", json::parse(R"({"branch":{"head":"main"},"entries":[{"path":"src/b.cpp","index":"M","worktree":"."}],"clean":false,"truncated":false})"));
	BOOST_TEST(model.selected() == "src/b.cpp");
	BOOST_TEST(model.selected_staged());
	BOOST_TEST(json::serialize(last("vcs.diff")->args) == R"({"path":"src/b.cpp","staged":true})");

	// Committed elsewhere: the file is in no list any more.
	model.refresh();
	finish("vcs.status", json::parse(R"({"branch":{"head":"main"},"entries":[],"clean":true,"truncated":false})"));
	BOOST_TEST(model.selected().empty());
}

BOOST_AUTO_TEST_CASE(AWriteThatEndsWhileHidden_ReadsNothingUntilShown)
{
	open_and_read();
	model.fetch();
	model.set_visible(false);
	const auto before = calls.size();
	finish("vcs.fetch", json::object{ { "output", "" } });
	BOOST_TEST(calls.size() == before);
	model.set_visible(true);
	BOOST_TEST(calls.size() == before + 3u);
}

BOOST_AUTO_TEST_CASE(ACancelledWrite_AsksNothing_AsAtTheEditorsShutdown)
{
	open_and_read();
	model.push();
	const auto before = calls.size();
	finish("vcs.push", json::value{}, edt::job_status::cancelled);
	BOOST_TEST(calls.size() == before);
	BOOST_TEST(model.busy().empty());
}

BOOST_AUTO_TEST_CASE(AnOlderDiffOrBranchesResult_IsIgnored)
{
	open_and_read();
	model.select("src/a.cpp", true);
	const auto first_diff = last("vcs.diff")->job;
	model.select("src/b.cpp", false);
	finish("vcs.diff", json::object{ { "text", "@@ -1 +1 @@\n+b\n" }, { "truncated", false } });
	events.job_finished.emit(first_diff, edt::job_status::done, std::string{},
		std::optional<json::value>{ json::object{ { "text", "@@ -1 +1 @@\n+a\n+a\n" }, { "truncated", false } } });
	BOOST_TEST(model.diff().size() == 2u);

	model.refresh();
	const auto first_branches = last("vcs.branches")->job;
	model.refresh();
	finish("vcs.branches", json::parse(R"({"branches":[{"name":"new","head":true,"remote":false}],"truncated":false})"));
	events.job_finished.emit(first_branches, edt::job_status::done, std::string{},
		std::optional<json::value>{ json::parse(R"({"branches":[{"name":"old","head":true,"remote":false}],"truncated":false})") });
	BOOST_REQUIRE(model.branches().size() == 1u);
	BOOST_TEST(model.branches()[0].name == "new");
}

BOOST_AUTO_TEST_CASE(OriginHead_IsNoBranchToSwitchTo_AndUntrackedFilesDoNotHoldASwitch)
{
	model.set_visible(true);
	finish("vcs.status", json::parse(R"({"branch":{"head":"main"},"entries":[{"path":"n.cpp","index":"?","worktree":"?"}],"clean":false,"truncated":false})"));
	finish("vcs.branches", json::parse(R"({"branches":[{"name":"main","head":true,"remote":false},{"name":"origin/HEAD","head":false,"remote":true},
		{"name":"origin/main","head":false,"remote":true}],"truncated":false})"));
	BOOST_TEST(model.branches().size() == 2u);
	BOOST_TEST(model.has_changes());
	BOOST_TEST(!model.has_tracked_changes());
}

BOOST_AUTO_TEST_CASE(AModelGoneBeforeItsJobEnds_HearsNothing)
{
	edt::editor_events own_events;
	auto gone = std::make_unique<edt::git_panel_model>(own_events, ports());
	gone->set_visible(true);
	const auto job = last("vcs.status")->job;
	gone.reset();
	own_events.job_finished.emit(job, edt::job_status::done, std::string{},
		std::optional<json::value>{ json::parse(R"({"branch":{"head":"x"},"entries":[],"clean":true,"truncated":false})") });
	BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(SelectingAFile_ShowsItsDiff_AndAnUntrackedOneSaysWhyNot)
{
	open_and_read();
	model.select("src/b.cpp", false);
	BOOST_TEST(json::serialize(last("vcs.diff")->args) == R"({"path":"src/b.cpp","staged":false})");
	finish("vcs.diff", json::object{ { "text", "diff --git a/src/b.cpp b/src/b.cpp\n@@ -1 +1 @@\n-old\n+new\n" }, { "truncated", false } });
	BOOST_REQUIRE(model.diff().size() == 4u);
	BOOST_TEST((model.diff()[2].type == edt::git_diff_line::kind::removed));

	const auto diffs = calls.size();
	model.select("new.cpp", false);
	BOOST_TEST(calls.size() == diffs);
	BOOST_TEST(model.diff().empty());
	BOOST_TEST(!model.diff_note().empty());
}

BOOST_AUTO_TEST_CASE(AnOlderStatus_DoesNotOverwriteANewerOne)
{
	model.set_visible(true);
	const auto first = last("vcs.status")->job;
	model.refresh();
	finish("vcs.status", json::parse(R"({"branch":{"head":"new"},"entries":[],"clean":true,"truncated":false})"));
	events.job_finished.emit(first, edt::job_status::done, std::string{},
		std::optional<json::value>{ json::parse(R"({"branch":{"head":"old"},"entries":[],"clean":true,"truncated":false})") });
	BOOST_TEST(model.head() == "new");
}

// #485: a change of the repository outside the editor (a commit in a terminal) reaches the panel through
// editor_events::repository_changed.
BOOST_AUTO_TEST_CASE(ARepositoryChange_RefreshesWithinASecond_WhileVisible)
{
	open_and_read();
	const auto status_before = last("vcs.status")->job;
	events.repository_changed.emit(edt::repository_change::refs);
	now += 100ms;
	model.tick();
	BOOST_TEST(last("vcs.status")->job == status_before);
	now += edt::git_panel_model::k_repo_delay;
	model.tick();
	BOOST_TEST(last("vcs.status")->job != status_before);
	BOOST_TEST((edt::git_panel_model::k_repo_delay + 100ms < 1s));
}

BOOST_AUTO_TEST_CASE(ARepositoryChangeWhileHidden_RunsNoGit_AndRefreshesWhenShown)
{
	open_and_read();
	model.set_visible(false);
	const auto before = calls.size();
	events.repository_changed.emit(edt::repository_change::refs);
	events.repository_changed.emit(edt::repository_change::index);
	now += 5s;
	model.tick();
	BOOST_TEST(calls.size() == before);
	BOOST_TEST(model.refs_changed() == 0u);
	model.set_visible(true);
	BOOST_TEST(calls.size() == before + 3u); // info, status, branches
	BOOST_TEST(model.refs_changed() == 1u);
}

BOOST_AUTO_TEST_CASE(AnIndexChange_RefreshesTheStatusOnly)
{
	open_and_read();
	const auto status_before = last("vcs.status")->job;
	events.repository_changed.emit(edt::repository_change::index);
	now += edt::git_panel_model::k_repo_delay;
	model.tick();
	BOOST_TEST(last("vcs.status")->job != status_before);
	BOOST_TEST(model.refs_changed() == 0u);
}

BOOST_AUTO_TEST_CASE(ARefsChange_RefreshesTheStatus_AndTheHistory)
{
	open_and_read();
	const auto status_before = last("vcs.status")->job;
	events.repository_changed.emit(edt::repository_change::refs);
	events.repository_changed.emit(edt::repository_change::index);
	now += edt::git_panel_model::k_repo_delay;
	model.tick();
	BOOST_TEST(last("vcs.status")->job != status_before);
	BOOST_TEST(model.refs_changed() == 1u);
	now += 5s;
	model.tick();
	BOOST_TEST(model.refs_changed() == 1u);
}

// The panel's own write already reloaded the History tab (writes_finished): the refs change it made is not a second reload.
BOOST_AUTO_TEST_CASE(TheRefsChangeOfThePanelsOwnWrite_DoesNotReloadTheHistoryAgain)
{
	open_and_read();
	model.fetch();
	finish("vcs.fetch", json::object{ { "output", "" } });
	BOOST_TEST(model.writes_finished() == 1u);
	now += 300ms;
	events.repository_changed.emit(edt::repository_change::refs);
	now += edt::git_panel_model::k_repo_delay;
	model.tick();
	BOOST_TEST(model.refs_changed() == 0u);

	// A later one (a commit in a terminal) is not the write's.
	events.repository_changed.emit(edt::repository_change::refs);
	now += edt::git_panel_model::k_repo_delay;
	model.tick();
	BOOST_TEST(model.refs_changed() == 1u);
}

BOOST_AUTO_TEST_CASE(ARefsChangeLongAfterAPanelWrite_ReloadsTheHistory)
{
	open_and_read();
	model.fetch();
	finish("vcs.fetch", json::object{ { "output", "" } });
	now += 10s;
	events.repository_changed.emit(edt::repository_change::refs);
	now += edt::git_panel_model::k_repo_delay;
	model.tick();
	BOOST_TEST(model.refs_changed() == 1u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_CASE(EdtGitDiffLines_HeadersEndAtTheFirstHunk)
{
	const auto lines = edt::diff_lines(
		"diff --git a/x b/x\nindex 1..2 100644\n--- a/x\n+++ b/x\n@@ -1,3 +1,3 @@\n context\n--- removed dashes\r\n+++ added plusses\n\\ No newline at end of file\n");
	using kind = edt::git_diff_line::kind;
	const std::vector<kind> expected{ kind::meta, kind::meta, kind::meta, kind::meta, kind::hunk, kind::context, kind::removed, kind::added, kind::note };
	BOOST_REQUIRE(lines.size() == expected.size());
	for (std::size_t i = 0; i < expected.size(); ++i) {
		BOOST_TEST_CONTEXT("line " << i) {
			BOOST_TEST((lines[i].type == expected[i]));
		}
	}
	BOOST_TEST(lines[6].text == "--- removed dashes");
}
