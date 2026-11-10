#include <boost/test/unit_test.hpp>
#include "edt_desc_problems.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/resource/res_tag.h>
#include <string>
#include <vector>

// #596: the source "desc" holds the diagnostics of the descs the editor loaded, one set per scope.

namespace {

res::diagnostic warning_at(std::string resource, std::string json_path, std::string reason)
{
	return res::diagnostic{ res::diagnostic_severity::warning, std::string{ res::diagnostic_kind::unknown_component }, std::move(resource),
		std::move(json_path), std::move(reason) };
}

std::vector<edt::problem> desc_problems(const edt::problem_list& problems)
{
	std::vector<edt::problem> out;
	for (const auto& entry : problems.list()) {
		if (entry.source == edt::desc_problem_publisher::source) out.push_back(entry.item);
	}
	return out;
}

struct publisher_fixture
{
	edt::editor_events events;
	edt::problem_list problems{ &events };
	edt::desc_problem_publisher publisher;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDescProblems, publisher_fixture)

BOOST_AUTO_TEST_CASE(ADiagnostic_IsARowWithTheResourceItsKindAndThePointerAsTheFirstNote)
{
	const edt::problem row = edt::desc_problem_publisher::to_problem(
		warning_at("res://levels/a.desc", "/worlds/0/children/1/components/x", "'nope' is not a registered desc type"));
	BOOST_TEST((row.severity == edt::problem_severity::warning));
	BOOST_TEST(row.file == "levels/a.desc");
	BOOST_TEST(row.code == "unknown_component");
	BOOST_TEST(row.message == "'nope' is not a registered desc type");
	BOOST_REQUIRE(row.notes.size() == 1u);
	BOOST_TEST(row.notes[0] == "/worlds/0/children/1/components/x");

	res::diagnostic error{ res::diagnostic_severity::error, std::string{ res::diagnostic_kind::load }, "memory://x/y.desc", "", "gone" };
	const edt::problem other = edt::desc_problem_publisher::to_problem(error);
	BOOST_TEST((other.severity == edt::problem_severity::error));
	BOOST_TEST(other.file == "memory://x/y.desc"); // not res://: the whole tag
	BOOST_TEST(other.notes.empty());
}

BOOST_AUTO_TEST_CASE(TheWorkingCopyOfALevel_IsShownAsItsFile)
{
	const res::tag copy{ "memory://editor/loading/3/a.desc" };
	const res::tag file{ "res://levels/a.desc" };
	publisher.report(problems, "level", { warning_at(copy.string(), "/worlds/0", "w"), warning_at("res://worlds/w.desc", "", "x") }, copy, file);
	const auto rows = desc_problems(problems);
	BOOST_REQUIRE(rows.size() == 2u);
	BOOST_TEST(rows[0].file == "levels/a.desc");
	BOOST_TEST(rows[1].file == "worlds/w.desc");
}

BOOST_AUTO_TEST_CASE(EachScope_ReplacesOnlyItsOwnRows_AndTheSourceGoesWhenAllAreClean)
{
	publisher.report(problems, "level", { warning_at("res://levels/a.desc", "/worlds/0", "level") });
	publisher.report(problems, "drop", { warning_at("res://props/b.desc", "", "drop") });
	BOOST_TEST(desc_problems(problems).size() == 2u);

	publisher.report(problems, "level", {}); // a clean reload of the level
	auto rows = desc_problems(problems);
	BOOST_REQUIRE(rows.size() == 1u);
	BOOST_TEST(rows[0].message == "drop");

	publisher.clear(problems, "drop");
	BOOST_TEST(desc_problems(problems).empty());
	for (const auto& entry : problems.list()) BOOST_TEST(entry.source != edt::desc_problem_publisher::source);

	publisher.clear(problems, "never_reported"); // nothing to drop: no error, nothing published
	BOOST_TEST(desc_problems(problems).empty());
}

BOOST_AUTO_TEST_CASE(WhatTheCollectorsDropped_IsOneRowForAllScopes)
{
	const res::diagnostic more_level{ res::diagnostic_severity::warning, std::string{ res::diagnostic_kind::truncated }, "", "", "12 more diagnostics" };
	const res::diagnostic more_drop{ res::diagnostic_severity::warning, std::string{ res::diagnostic_kind::truncated }, "", "", "3 more diagnostics" };
	publisher.report(problems, "level", { warning_at("res://levels/a.desc", "", "a"), more_level });
	publisher.report(problems, "drop", { more_drop });
	const auto rows = desc_problems(problems);
	BOOST_REQUIRE(rows.size() == 2u);
	BOOST_TEST(rows[0].message == "a");
	BOOST_TEST(rows[1].code == "truncated");
	BOOST_TEST(rows[1].message == "15 more diagnostics truncated");
}

BOOST_AUTO_TEST_CASE(AReportOfTheSameRows_SetsNothing_AndADropReplacesTheLastDrop)
{
	publisher.report(problems, "drop", { warning_at("res://props/a.desc", "", "first") });
	const auto generation = problems.generation();
	publisher.report(problems, "drop", { warning_at("res://props/a.desc", "", "first") });
	BOOST_TEST(problems.generation() == generation); // the panel keeps its selection

	publisher.report(problems, "drop", { warning_at("res://props/b.desc", "", "second") });
	const auto rows = desc_problems(problems);
	BOOST_REQUIRE(rows.size() == 1u);
	BOOST_TEST(rows[0].message == "second");
}

BOOST_AUTO_TEST_SUITE_END()
