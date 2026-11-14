#include <boost/test/unit_test.hpp>
#include "edt_agent_policy.h"
#include "edt_migration_window_model.h"
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <boost/json/value_to.hpp>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Project Migration window's model (EPIC-58 S8, #761): plan -> migrate -> report with the two commands and job_finished
// faked; refusals at the call and in the job; Close. And the action that opens the window is a person's only.
namespace {

namespace json = boost::json;
using phase = edt::migration_window_model::phase;

constexpr const char* k_plan = R"({"code":1,"text":"plan text","fingerprint":"0123456789abcdef","plan":{
	"files":[{"path":"res/cameras/cam_v1.desc","steps":[{"type":"camera_desc","from":1,"to":2,"summary":"drops the pose"}],
		"warnings":[{"type":"camera_desc","from":1,"to":2,"message":"the pose is lost"}]},
		{"path":"res/levels/old.desc","steps":[{"type":"level_desc","from":1,"to":2,"summary":"worlds by name"}],"warnings":[]}],
	"project_steps":[{"id":"engine.manifest_format","phase":"post","summary":"format 2"}],
	"manifest":{"engine_version":"0.1.0","outdated":true,"completes":true},
	"skipped":[{"path":"res/newer.desc","kind":"newer","reason":"version 99"}]}})";

constexpr const char* k_report = R"({"code":4,"status":"partial","id":"20261114T000000Z-0.2.0","text":"# report",
	"report":{"files":[{"path":"res/cameras/cam_v1.desc","warnings":[{"type":"camera_desc","from":1,"message":"the pose is lost"}]}]},
	"level":{"tag":"res://levels/old.desc","reopened":true}})";

struct window_fixture
{
	struct call
	{
		std::string id;
		json::value args;
	};

	edt::editor_events events;
	std::vector<call> calls;
	std::vector<std::string> opened;
	std::optional<cmd::result> plan_answer;
	std::optional<cmd::result> migrate_answer;
	std::uint64_t job = 7;
	edt::migration_window_model model{ events, ports() };

	edt::migration_window_model::ports ports()
	{
		edt::migration_window_model::ports out;
		out.invoke = [this](std::string_view id, json::value args) {
			calls.push_back({ std::string(id), std::move(args) });
			if (id == "project.migration_plan") return plan_answer ? *plan_answer : cmd::result::success(json::parse(k_plan));
			if (migrate_answer) return *migrate_answer;
			return cmd::result::success(json::object{ { "job", job } });
		};
		out.open_file = [this](const std::string& path) { opened.push_back(path); };
		return out;
	}

	void plan()
	{
		model.request_plan();
		BOOST_TEST((model.state() == phase::planning));
		model.tick();
	}

	void finish(json::value result, edt::job_status status = edt::job_status::done, std::uint64_t id = 0)
	{
		events.job_finished.emit(id == 0 ? job : id, status, std::string{ "it broke" }, std::optional<json::value>{ std::move(result) });
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtMigrationWindowModel, window_fixture)

BOOST_AUTO_TEST_CASE(Plan_Migrate_Report)
{
	BOOST_TEST((model.state() == phase::idle));
	BOOST_TEST(!model.can_migrate());
	plan();
	BOOST_TEST_REQUIRE((model.state() == phase::plan_shown));
	BOOST_TEST(calls.size() == 1u);
	BOOST_TEST(calls[0].id == "project.migration_plan");
	const auto& shown = *model.plan();
	BOOST_TEST(shown.code == 1);
	BOOST_TEST_REQUIRE(shown.files.size() == 2u);
	BOOST_TEST(shown.files[0].path == "res/cameras/cam_v1.desc");
	BOOST_TEST(shown.files[0].steps.at(0) == "camera_desc v1 -> v2: drops the pose");
	BOOST_TEST(shown.files[0].warnings.at(0) == "camera_desc v1: the pose is lost");
	BOOST_TEST(shown.files[1].warnings.empty());
	BOOST_TEST(shown.project_steps.at(0) == "engine.manifest_format (post): format 2");
	BOOST_TEST(shown.skipped.at(0) == "res/newer.desc (newer): version 99");
	BOOST_TEST(shown.manifest == "Engine 0.1.0 -> this engine");
	BOOST_TEST(shown.text == "plan text");
	BOOST_TEST(model.can_migrate());

	model.open_file(shown.files[1].path);
	BOOST_TEST((opened == std::vector<std::string>{ "res/levels/old.desc" }));

	model.migrate();
	BOOST_TEST((model.state() == phase::running));
	BOOST_TEST(calls.back().id == "project.migrate");
	BOOST_TEST((model.job() == std::optional<std::uint64_t>{ job }));
	BOOST_TEST(!model.can_migrate());
	// Close and Refresh wait for the end of the run.
	model.close();
	model.request_plan();
	BOOST_TEST((model.state() == phase::running));
	// Another job's end is not this run's.
	finish(json::parse(k_report), edt::job_status::done, job + 1);
	BOOST_TEST((model.state() == phase::running));

	finish(json::parse(k_report));
	BOOST_TEST_REQUIRE((model.state() == phase::report_shown));
	const auto& report = *model.report();
	BOOST_TEST(report.code == 4);
	BOOST_TEST(report.status == "partial");
	BOOST_TEST(report.id == "20261114T000000Z-0.2.0");
	BOOST_TEST(report.text == "# report");
	BOOST_TEST(report.warnings.at(0) == "res/cameras/cam_v1.desc: camera_desc v1: the pose is lost");
	BOOST_TEST(report.level == "Level res://levels/old.desc reopened");
	BOOST_TEST(report.error_code.empty());
	BOOST_TEST(!model.job().has_value());
}

BOOST_AUTO_TEST_CASE(ARefusedPlan_DisablesMigrate)
{
	plan_answer = cmd::result::success(json::parse(R"({"code":2,"text":"","plan":{"files":[],"project_steps":[],"skipped":[],
		"refused":"res/broken.desc is not valid JSON","unfinished":["20260101T000000Z-0.1.0"]}})"));
	plan();
	BOOST_TEST_REQUIRE((model.state() == phase::plan_shown));
	BOOST_TEST(model.plan()->refused == "res/broken.desc is not valid JSON");
	BOOST_TEST(model.plan()->unfinished.size() == 1u);
	BOOST_TEST(!model.can_migrate());
	model.migrate();
	BOOST_TEST(calls.size() == 1u);

	// Nothing to do and only skipped files cannot be migrated either.
	plan_answer = cmd::result::success(json::parse(R"({"code":0,"text":"","plan":{}})"));
	plan();
	BOOST_TEST(!model.can_migrate());
	plan_answer = cmd::result::success(json::parse(R"({"code":4,"text":"","plan":{}})"));
	plan();
	BOOST_TEST(!model.can_migrate());
}

BOOST_AUTO_TEST_CASE(APlanThatFails_ShowsWhy)
{
	plan_answer = cmd::result::failure("no_project", "no project is open");
	plan();
	BOOST_TEST((model.state() == phase::idle));
	BOOST_TEST(!model.plan().has_value());
	BOOST_TEST(model.error_code() == "no_project");
	BOOST_TEST(model.error_text() == "no project is open");
	BOOST_TEST(!model.error_hint().empty());

	// A good plan clears it.
	plan_answer.reset();
	plan();
	BOOST_TEST((model.state() == phase::plan_shown));
	BOOST_TEST(model.error_code().empty());
}

BOOST_AUTO_TEST_CASE(AMigrationRefusedAtOnce_KeepsThePlanWithTheReason)
{
	plan();
	migrate_answer = cmd::result::failure(cmd::codes::busy, "Play is running: stop it first");
	model.migrate();
	BOOST_TEST((model.state() == phase::plan_shown));
	BOOST_TEST(model.error_code() == "busy");
	BOOST_TEST(model.error_text() == "Play is running: stop it first");
	BOOST_TEST(!model.error_hint().empty());
	BOOST_TEST(model.can_migrate());

	migrate_answer = cmd::result::failure(cmd::codes::needs_input, "the level has unsaved changes: save or discard them first");
	model.migrate();
	BOOST_TEST(model.error_code() == "needs_input");
	BOOST_TEST(model.error_hint().find("unsaved") != std::string::npos);

	// The next try that starts clears the reason.
	migrate_answer.reset();
	model.migrate();
	BOOST_TEST((model.state() == phase::running));
	BOOST_TEST(model.error_code().empty());
}

BOOST_AUTO_TEST_CASE(AJobThatAnswersWithAnError_ShowsIt)
{
	plan();
	model.migrate();
	finish(json::parse(R"({"error":{"code":"denied","message":"the project changed since the question: ask again"}})"));
	BOOST_TEST_REQUIRE((model.state() == phase::report_shown));
	BOOST_TEST(model.report()->code == -1);
	BOOST_TEST(model.report()->error_code == "denied");
	BOOST_TEST(model.report()->error_text == "the project changed since the question: ask again");

	// A failed or cancelled job, and code 6 with the reason as text.
	model.close();
	plan();
	model.migrate();
	finish(json::value{}, edt::job_status::failed);
	BOOST_TEST(model.report()->error_code == "failed");
	BOOST_TEST(model.report()->error_text == "it broke");

	model.close();
	plan();
	model.migrate();
	finish(json::parse(R"({"code":6,"status":"rollback_failed","error":"the migration stopped: boom","text":"t",
		"level":{"tag":"res://levels/old.desc","reopened":false,"reason":"left detached"}})"));
	BOOST_TEST(model.report()->code == 6);
	BOOST_TEST(model.report()->error_text == "the migration stopped: boom");
	BOOST_TEST(model.report()->level == "Level res://levels/old.desc not reopened: left detached");
}

// Only the plan shown is applied: project.migrate gets its fingerprint.
BOOST_AUTO_TEST_CASE(Migrate_PassesTheFingerprintOfThePlanShown)
{
	plan();
	BOOST_TEST(model.plan()->fingerprint == "0123456789abcdef");
	model.migrate();
	BOOST_REQUIRE(calls.back().id == "project.migrate");
	BOOST_TEST(json::value_to<std::string>(calls.back().args.at("plan_fingerprint")) == "0123456789abcdef");
}

// The project changed between the plan shown and the run: nothing was written; the plan is made again and says why.
BOOST_AUTO_TEST_CASE(APlanChangedAnswer_PlansAgain_WithANotice)
{
	plan();
	model.migrate();
	finish(json::parse(R"({"error":{"code":"plan_changed","message":"the project changed since the plan was shown: nothing was written"}})"));
	BOOST_TEST((model.state() == phase::planning));
	BOOST_TEST(!model.report().has_value());
	BOOST_TEST(!model.notice().empty());
	model.tick();
	BOOST_TEST_REQUIRE((model.state() == phase::plan_shown));
	BOOST_TEST(model.notice().find("the plan is refreshed") != std::string::npos);
	BOOST_TEST(std::ranges::count_if(calls, [](const call& made) { return made.id == "project.migration_plan"; }) == 2);
	// A Refresh of the person's own clears it.
	model.request_plan();
	BOOST_TEST(model.notice().empty());
}

// The report stays until Close: opening the window again (which asks for a plan) does not replace it.
BOOST_AUTO_TEST_CASE(TheReport_StaysWhenTheWindowIsOpenedAgain)
{
	plan();
	model.migrate();
	finish(json::parse(k_report));
	BOOST_TEST_REQUIRE((model.state() == phase::report_shown));
	model.request_plan();
	model.tick();
	BOOST_TEST((model.state() == phase::report_shown));
	BOOST_TEST(model.report()->id == "20261114T000000Z-0.2.0");
	model.close();
	model.request_plan();
	BOOST_TEST((model.state() == phase::planning));
}

BOOST_AUTO_TEST_CASE(Close_GoesBackToIdle)
{
	plan();
	model.migrate();
	finish(json::parse(k_report));
	model.close();
	BOOST_TEST((model.state() == phase::idle));
	BOOST_TEST(!model.plan().has_value());
	BOOST_TEST(!model.report().has_value());
	BOOST_TEST(model.error_code().empty());
	// A late answer of a job that is no longer followed changes nothing.
	finish(json::parse(k_report));
	BOOST_TEST((model.state() == phase::idle));
}

BOOST_AUTO_TEST_CASE(TextFromTheProject_HasNoControlCharacters)
{
	plan_answer = cmd::result::success(json::parse(R"({"code":1,"text":"a\u001b[31mb\nc","plan":{"files":[{"path":"res/x\u0007.desc","steps":[],"warnings":[]}]}})"));
	plan();
	BOOST_TEST(model.plan()->files.at(0).path == "res/x .desc");
	BOOST_TEST(model.plan()->text == "a [31mb\nc");
}

// #761: a migration that failed (code 5 or 6) or an unfinished one in the plan offers Roll Back; it runs
// project.migrate_rollback {id} as a person's call and shows its answer as a rollback's.
BOOST_AUTO_TEST_CASE(RollBack_IsOfferedForAFailedOrUnfinishedMigration_AndShowsItsAnswer)
{
	plan();
	BOOST_TEST(!model.can_roll_back());
	model.migrate();
	finish(json::parse(R"({"code":4,"status":"partial","id":"m1","text":"t"})"));
	BOOST_TEST(!model.can_roll_back());
	model.close();

	plan();
	model.migrate();
	finish(json::parse(R"({"code":6,"status":"rollback_failed","error":"stopped","unfinished":["m2"],"text":"t"})"));
	BOOST_TEST_REQUIRE(model.can_roll_back());
	BOOST_TEST(model.rollback_id() == "m2");
	model.roll_back(model.rollback_id());
	BOOST_TEST((model.state() == phase::running));
	BOOST_TEST(model.rolling_back());
	BOOST_REQUIRE(!calls.empty());
	BOOST_TEST(calls.back().id == "project.migrate_rollback");
	BOOST_TEST(json::serialize(calls.back().args) == R"({"id":"m2"})");
	finish(json::parse(R"({"code":0,"status":"rolled_back","id":"m2","text":"migration m2 rolled back","level":{"tag":"","reopened":false,"reason":"no level was open"}})"));
	BOOST_TEST_REQUIRE((model.state() == phase::report_shown));
	BOOST_TEST(model.report()->rollback);
	BOOST_TEST(model.report()->code == 0);
	BOOST_TEST(!model.rolling_back());
	BOOST_TEST(!model.can_roll_back());
	BOOST_TEST(edt::migration_window_model::rollback_outcome_text(0).find("Rolled back") != std::string::npos);
	model.close();

	// The plan names an unfinished migration: Roll Back is offered there too; a refusal at once keeps the plan.
	plan_answer = cmd::result::success(json::parse(R"({"code":2,"text":"t","fingerprint":"","plan":{"files":[],"project_steps":[],
		"skipped":[],"refused":"migration m3 did not finish","unfinished":["m3"]}})"));
	plan();
	BOOST_TEST(model.rollback_id() == "m3");
	migrate_answer = cmd::result::failure(cmd::codes::busy, "Play is running");
	model.roll_back("m3");
	BOOST_TEST((model.state() == phase::plan_shown));
	BOOST_TEST(model.error_code() == cmd::codes::busy);
}

BOOST_AUTO_TEST_CASE(TheOpenAction_IsForAPersonOnly)
{
	edt::action_registry actions;
	bool project = false;
	int opened_windows = 0;
	const edt::registration registered = edt::register_migration_window_action(actions, [&project] { return project; },
		[&opened_windows] { ++opened_windows; });
	const cmd::command_desc* desc = actions.find("project.migrate.open");
	BOOST_REQUIRE(desc != nullptr);
	BOOST_TEST((desc->category == cmd::agent_category::not_for_agents));
	BOOST_TEST(!desc->expose_as_tool);
	for (const cmd::command_desc* tool : actions.tools()) BOOST_TEST(tool->id != "project.migrate.open");

	const edt::policy_layer none{};
	BOOST_TEST(((edt::effective_mode(*desc, none, none).mode) == cmd::policy_mode::deny));
	edt::policy_layer allowing;
	allowing.categories[cmd::agent_category::not_for_agents] = cmd::policy_mode::allow;
	allowing.commands["project.migrate.open"] = cmd::policy_mode::allow;
	BOOST_TEST(((edt::effective_mode(*desc, allowing, allowing).mode) == cmd::policy_mode::deny));

	// In the Project menu, enabled only with a project open.
	const auto items = actions.items("Project");
	BOOST_REQUIRE(items.size() == 1u);
	BOOST_TEST(items[0].label == "Migrate Project...");
	BOOST_TEST(!items[0].enabled);
	BOOST_TEST(!actions.invoke("project.migrate.open", json::object{}).ok);
	BOOST_TEST(opened_windows == 0);
	project = true;
	BOOST_TEST(actions.items("Project")[0].enabled);
	BOOST_TEST(actions.invoke("project.migrate.open", json::object{}).ok);
	BOOST_TEST(opened_windows == 1);
}

BOOST_AUTO_TEST_SUITE_END()
