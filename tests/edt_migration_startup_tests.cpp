#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include "edt_migration_startup.h"
#include <snk/project_migration/pmg_asset_steps.h>
#include <snk/project_migration/pmg_scan.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

// What the editor does about a project's migrations when it opens (#761): a migration it was left with is offered for a
// rollback (a person is asked, headless a WARN names the command line); a background scan's result is shown as a notice
// unless the person said Later for that project and that set (kept in the person's settings, here a string).
namespace {

namespace pmg = prj::migration;
using notice = edt::migration_notice_model;

struct notice_fixture
{
	std::optional<std::string> stored;
	int saves = 0;

	notice::ports ports()
	{
		notice::ports out;
		out.load = [this] { return stored; };
		out.save = [this](const std::string& text) -> std::optional<std::string> {
			stored = text;
			++saves;
			return std::nullopt;
		};
		return out;
	}

	static edt::migration_scan_summary summary(std::string set, std::size_t outdated = 3)
	{
		edt::migration_scan_summary out;
		out.outdated = outdated;
		out.set = std::move(set);
		return out;
	}
};

struct scan_fixture : test_util::scene_env
{
	test_util::fixture_copy copy{ "migration_v1" };
};

}

BOOST_AUTO_TEST_SUITE(EdtMigrationStartup)

BOOST_AUTO_TEST_CASE(NothingUnfinished_OffersNothing)
{
	BOOST_TEST((edt::decide_journal_offer({}, false).what == edt::journal_offer::kind::none));
	BOOST_TEST((edt::decide_journal_offer({}, true).what == edt::journal_offer::kind::none));
}

BOOST_AUTO_TEST_CASE(APerson_IsAskedAboutTheFirst_HeadlessGetsTheCommandLine)
{
	const std::vector<std::string> unfinished{ "20260101T000000Z-0.1.0", "20260202T000000Z-0.2.0" };
	const edt::journal_offer asked = edt::decide_journal_offer(unfinished, false);
	BOOST_TEST((asked.what == edt::journal_offer::kind::ask));
	BOOST_TEST(asked.id == unfinished.front());
	BOOST_TEST(asked.title == "Migration 20260101T000000Z-0.1.0 did not finish");
	BOOST_TEST(asked.text.find("partly migrated") != std::string::npos, asked.text);
	BOOST_TEST(asked.text.find("1 more") != std::string::npos, asked.text);

	const edt::journal_offer warned = edt::decide_journal_offer(unfinished, true);
	BOOST_TEST((warned.what == edt::journal_offer::kind::warn));
	BOOST_TEST(warned.id == unfinished.front());
	BOOST_TEST(warned.text.find("--migrate-rollback 20260101T000000Z-0.1.0") != std::string::npos, warned.text);
}

// A folder name that is no migration id is not offered for a rollback (the command would refuse it): the person is told
// why, and the text has neither control characters nor '#' (a dialog's title is its ImGui name).
BOOST_AUTO_TEST_CASE(AnInvalidId_IsNotOfferedForARollback_ItsReasonIsShown)
{
	for (const std::string id : { std::string("a\nb\x1b"), std::string("x###y"), std::string(".hidden") }) {
		BOOST_TEST_CONTEXT(id) {
			const edt::journal_offer told = edt::decide_journal_offer({ id }, false);
			BOOST_TEST((told.what == edt::journal_offer::kind::inform));
			BOOST_TEST(told.text.find("cannot be rolled back") != std::string::npos, told.text);
			BOOST_TEST(told.text.find('\n') == std::string::npos);
			BOOST_TEST(told.text.find('\x1b') == std::string::npos);
			BOOST_TEST(told.text.find('#') == std::string::npos);
			BOOST_TEST(told.title.find('#') == std::string::npos);
			BOOST_TEST((edt::decide_journal_offer({ id }, true).what == edt::journal_offer::kind::warn));
		}
	}
}

// The checks of an opened project run once, after the start level settled; headless only a WARN: no question, no scan.
BOOST_AUTO_TEST_CASE(TheStartupChecks_AskOnce_AndHeadlessAsksNothing)
{
	bool settled = false;
	int asked = 0;
	int warned = 0;
	int scans = 0;
	const auto ports = [&] {
		edt::migration_startup::ports out;
		out.settled = [&settled] { return settled; };
		out.unfinished = [] { return std::vector<std::string>{ "20260101T000000Z-0.1.0" }; };
		out.ask = [&asked](const edt::journal_offer& offer) {
			BOOST_TEST((offer.what == edt::journal_offer::kind::ask));
			++asked;
		};
		out.warn = [&warned](const std::string&) { ++warned; };
		out.start_scan = [&scans] { ++scans; };
		return out;
	};

	edt::migration_startup person{ ports(), false };
	BOOST_TEST(person.tick());
	BOOST_TEST(person.tick());
	BOOST_TEST(asked == 0);
	BOOST_TEST(scans == 0);
	settled = true;
	BOOST_TEST(!person.tick());
	BOOST_TEST(!person.tick());
	BOOST_TEST(asked == 1);
	BOOST_TEST(scans == 1);

	asked = warned = scans = 0;
	settled = false;
	edt::migration_startup headless{ ports(), true };
	BOOST_TEST(!headless.tick());
	BOOST_TEST(!headless.tick());
	BOOST_TEST(asked == 0);
	BOOST_TEST(scans == 0);
	BOOST_TEST(warned == 1);
}

// The notice's Later goes to the person's settings: the last edt:// root (else the user data folder), never the project.
BOOST_AUTO_TEST_CASE(TheNoticeSettings_AreThePersons_NotTheProjects)
{
	const std::filesystem::path project = std::filesystem::path("work") / "game";
	const std::filesystem::path user = std::filesystem::path("home") / "editor";
	const std::filesystem::path file = edt::migration_notice_file({ std::filesystem::path("data") / "edt", user }, project);
	BOOST_TEST((file == user / "settings" / "migration_notice.json"));
	BOOST_TEST((edt::migration_notice_file({}, user) == user / "settings" / "migration_notice.json"));
	const auto relative = file.lexically_relative(project);
	BOOST_TEST((relative.empty() || *relative.begin() == ".."));
}

// The count is what project.migration_plan's code 1 rests on: the outdated files, the project steps the scan needs, and
// the manifest only when no file of an unloaded module is left.
BOOST_FIXTURE_TEST_CASE(TheSummary_CountsWhatThePlanWouldMigrate, scan_fixture)
{
	const pmg::scan_result scan = pmg::scan_project(copy.root(), pmg::type_catalog::from(*descs));
	pmg::project_step_registry steps;
	pmg::register_project_steps(steps);
	const edt::migration_scan_summary summary = edt::summarize_scan(scan, steps);
	BOOST_TEST(summary.outdated == scan.count(pmg::file_class::outdated));
	BOOST_TEST(summary.outdated > 0u);
	BOOST_TEST((std::ranges::find(summary.steps, std::string{ "assets.ids" }) != summary.steps.end()));
	// res/module/unknown.desc stays: the engine version is not stamped.
	BOOST_TEST(!summary.manifest);
	BOOST_TEST(summary.needed());
	BOOST_TEST(summary.set.size() == 16u);
	BOOST_TEST(edt::summarize_scan(scan, steps).set == summary.set);

	pmg::scan_result fewer = scan;
	std::erase_if(fewer.files, [](const pmg::scanned_file& file) { return file.path == "res/cameras/cam_v1.desc"; });
	BOOST_TEST(edt::summarize_scan(fewer, steps).set != summary.set);
	BOOST_TEST(!edt::summarize_scan(pmg::scan_result{}, pmg::project_step_registry{}).needed());
	BOOST_TEST(summary.complete);
}

// The scan stops between two entries once its stop says so (the job's token: cancel, the editor quitting): it ends early,
// marked incomplete, and its summary is never shown.
BOOST_FIXTURE_TEST_CASE(AStoppedScan_EndsEarly_AndIsDropped, scan_fixture)
{
	const pmg::type_catalog catalog = pmg::type_catalog::from(*descs);
	const pmg::scan_result whole = pmg::scan_project(copy.root(), catalog);
	BOOST_REQUIRE(whole.files.size() > 4u);
	BOOST_TEST(!whole.incomplete);

	int asked = 0;
	pmg::scan_options options;
	options.stop = [&asked] { return ++asked > 3; };
	const pmg::scan_result cut = pmg::scan_project(copy.root(), catalog, options);
	BOOST_TEST(cut.incomplete);
	BOOST_TEST(cut.files.size() < whole.files.size());

	pmg::project_step_registry steps;
	pmg::register_project_steps(steps);
	const edt::migration_scan_summary summary = edt::summarize_scan(cut, steps);
	BOOST_TEST(!summary.complete);
	notice_fixture settings;
	notice model{ settings.ports() };
	BOOST_TEST(!model.deliver({ "/p", 0, 0 }, { "/p", 0, 0 }, summary));
	BOOST_TEST(!model.visible());
}

BOOST_FIXTURE_TEST_CASE(AScanOfAnotherCodeOrProject_IsDropped, notice_fixture)
{
	notice model{ ports() };
	BOOST_TEST(!model.deliver({ "/p", 1 }, { "/p", 2 }, summary("a")));
	BOOST_TEST(!model.visible());
	BOOST_TEST(!model.deliver({ "/p", 1 }, { "/q", 1 }, summary("a")));
	BOOST_TEST(!model.visible());
	BOOST_TEST(model.deliver({ "/p", 1 }, { "/p", 1 }, summary("a")));
	BOOST_TEST(model.visible());
	BOOST_TEST(model.text() == "3 files of this project are in an old format.");
	BOOST_TEST(saves == 0);
}

// A migration or rollback that started after the scan, or a stop that cut it short: the result is dropped. A migration left
// unfinished keeps the notice back (its rollback question goes first).
BOOST_FIXTURE_TEST_CASE(AStaleStoppedOrUnfinishedScan_ShowsNothing, notice_fixture)
{
	notice model{ ports() };
	BOOST_TEST(!model.deliver({ "/p", 1, 0 }, { "/p", 1, 1 }, summary("a")));
	BOOST_TEST(!model.visible());
	edt::migration_scan_summary stopped = summary("a");
	stopped.complete = false;
	BOOST_TEST(!model.deliver({ "/p", 1, 1 }, { "/p", 1, 1 }, stopped));
	BOOST_TEST(!model.visible());
	edt::migration_scan_summary unfinished = summary("a");
	unfinished.unfinished = { "m1" };
	BOOST_TEST(model.deliver({ "/p", 1, 1 }, { "/p", 1, 1 }, unfinished));
	BOOST_TEST(!model.visible());
}

BOOST_FIXTURE_TEST_CASE(NothingNeeded_ShowsNothing, notice_fixture)
{
	notice model{ ports() };
	BOOST_TEST(model.deliver({ "/p", 0 }, { "/p", 0 }, summary("a", 0)));
	BOOST_TEST(!model.visible());
}

// Later is kept per project and per set: the same set stays quiet, another set or another project shows it again.
BOOST_FIXTURE_TEST_CASE(Later_IsKeptPerProjectAndSet, notice_fixture)
{
	{
		notice model{ ports() };
		BOOST_REQUIRE(model.deliver({ "/p", 0 }, { "/p", 0 }, summary("a")));
		BOOST_TEST(!model.later().has_value());
		BOOST_TEST(!model.visible());
	}
	BOOST_REQUIRE(stored.has_value());
	const auto entries = notice::parse_later(*stored);
	BOOST_REQUIRE(entries.size() == 1u);
	BOOST_TEST(entries[0].project == "/p");
	BOOST_TEST(entries[0].set == "a");

	notice same{ ports() };
	same.deliver({ "/p", 7 }, { "/p", 7 }, summary("a"));
	BOOST_TEST(!same.visible());
	notice changed{ ports() };
	changed.deliver({ "/p", 0 }, { "/p", 0 }, summary("b"));
	BOOST_TEST(changed.visible());
	notice other{ ports() };
	other.deliver({ "/q", 0 }, { "/q", 0 }, summary("a"));
	BOOST_TEST(other.visible());

	// A new Later of the project replaces its entry; Open hides it for the session only.
	changed.later();
	BOOST_TEST(notice::parse_later(*stored).size() == 1u);
	BOOST_TEST(notice::parse_later(*stored)[0].set == "b");
	other.open();
	BOOST_TEST(!other.visible());
	BOOST_TEST(notice::parse_later(*stored).size() == 1u);
}

BOOST_FIXTURE_TEST_CASE(TheSettings_KeepTheNewestProjects_AndABrokenFileIsNone, notice_fixture)
{
	for (std::size_t i = 0; i < notice::k_max_remembered + 5; ++i) {
		notice model{ ports() };
		const std::string project = std::format("/p{}", i);
		model.deliver({ project, 0 }, { project, 0 }, summary("a"));
		model.later();
	}
	const auto entries = notice::parse_later(*stored);
	BOOST_TEST(entries.size() == notice::k_max_remembered);
	BOOST_TEST(entries.back().project == std::format("/p{}", notice::k_max_remembered + 4));
	BOOST_TEST(entries.front().project == "/p5");

	for (const char* text : { "", "not json", "[]", R"({"later":5})", R"({"later":[1,{"project":2,"set":"a"}]})" }) {
		BOOST_TEST_CONTEXT(text) {
			BOOST_TEST(notice::parse_later(text).empty());
		}
	}
	stored = "not json";
	notice model{ ports() };
	model.deliver({ "/p", 0 }, { "/p", 0 }, summary("a"));
	BOOST_TEST(model.visible());
}

BOOST_AUTO_TEST_SUITE_END()
