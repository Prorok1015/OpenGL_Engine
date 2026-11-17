#include <boost/test/unit_test.hpp>
#include "edt_input_actions_view.h"
#include <snk/input/inp_events.hpp>

// The Input Actions panel without ImGui (US-542-C): rows, what changed, the wait for a key, the words for an answer.

namespace {

using KB = inp::KEYBOARD_BUTTONS;
using MB = inp::MOUSE_BUTTONS;
namespace view = edt::input_actions_view;

inp::action_map project_map()
{
	inp::action_map map;
	map.add("MoveLeft", { KB::A, KB::LEFT });
	map.add("Fire", { MB::LEFT });
	map.add("Jump", { inp::GAMEPAD_BUTTONS::A, inp::GAMEPAD_AXES::LEFT_X_NEG });
	return map;
}

}

BOOST_AUTO_TEST_SUITE(EdtInputActionsViewTests)

BOOST_AUTO_TEST_CASE(EveryKindOfBinding_HasItsText)
{
	BOOST_TEST(view::binding_text(KB::A) == "A");
	BOOST_TEST(view::binding_text(MB::RIGHT) == "Mouse RIGHT");
	BOOST_TEST(view::binding_text(inp::GAMEPAD_BUTTONS::DPAD_UP) == "Pad DPAD_UP");
	BOOST_TEST(view::binding_text(inp::GAMEPAD_AXES::LEFT_X_NEG) == "Axis LeftX-");
}

BOOST_AUTO_TEST_CASE(RowsFollowTheProjectsOrder_AndMarkWhatTheLayerChanged)
{
	const inp::action_map project = project_map();
	inp::user_bindings user;
	BOOST_REQUIRE(user.rebind(project, "MoveLeft", 1, KB::Q).ok());
	BOOST_REQUIRE(user.rebind(project, "Fire", 1, MB::RIGHT).ok());
	const inp::action_map effective = user.apply(project);

	const auto rows = view::build_rows(project, effective, {});
	BOOST_REQUIRE_EQUAL(rows.size(), 3u);
	BOOST_TEST(rows[0].name == "MoveLeft");
	BOOST_TEST(rows[0].changed);
	BOOST_REQUIRE_EQUAL(rows[0].bindings.size(), 2u);
	BOOST_TEST(!rows[0].bindings[0].changed);
	BOOST_TEST(rows[0].bindings[1].changed);
	BOOST_TEST(rows[0].bindings[1].text == "Q");
	BOOST_TEST(rows[1].changed);
	BOOST_TEST(rows[1].bindings[1].changed);
	BOOST_TEST(!rows[2].changed);
}

BOOST_AUTO_TEST_CASE(ASlotTakenOut_MarksTheActionChanged)
{
	const inp::action_map project = project_map();
	inp::user_bindings user;
	BOOST_REQUIRE(user.remove(project, "MoveLeft", 1).ok());
	const auto rows = view::build_rows(project, user.apply(project), {});
	BOOST_TEST(rows[0].changed);
	BOOST_TEST(rows[0].bindings.size() == 1u);
	BOOST_TEST(!rows[0].bindings[0].changed);
}

BOOST_AUTO_TEST_CASE(ConflictsNameTheOtherActionOnBothRows_OnceEach)
{
	const inp::action_map project = project_map();
	inp::user_bindings user;
	BOOST_REQUIRE(user.rebind(project, "Fire", 0, KB::A, true).ok());
	BOOST_REQUIRE(user.rebind(project, "Fire", 1, KB::LEFT, true).ok());
	const inp::action_map effective = user.apply(project);
	const auto conflicts = inp::find_binding_conflicts(effective);
	BOOST_REQUIRE_EQUAL(conflicts.size(), 2u);

	const auto rows = view::build_rows(project, effective, conflicts);
	BOOST_TEST((rows[0].shares_with == std::vector<std::string>{ "Fire" }));
	BOOST_TEST((rows[1].shares_with == std::vector<std::string>{ "MoveLeft" }));
	BOOST_TEST(rows[2].shares_with.empty());
}

BOOST_AUTO_TEST_CASE(EveryConflictIsAWarningProblemThatNamesTheBinding)
{
	const std::vector<inp::binding_conflict> conflicts{ { KB::A, "MoveLeft", "Fire" } };
	const auto problems = view::conflict_problems(conflicts);
	BOOST_REQUIRE_EQUAL(problems.size(), 1u);
	BOOST_TEST((problems[0].severity == edt::problem_severity::warning));
	BOOST_TEST(problems[0].message == "Input actions 'MoveLeft' and 'Fire' share A");
	BOOST_TEST(view::conflict_problems({}).empty());
}

BOOST_AUTO_TEST_CASE(TheCapture_WaitsOnOneSlot_AndEndsWhenToldTo)
{
	view::capture capture;
	BOOST_TEST(!capture.active());
	BOOST_TEST(!capture.waits_on("Jump", 0));

	capture.begin("Jump", 1);
	BOOST_TEST(capture.active());
	BOOST_TEST(capture.waits_on("Jump", 1));
	BOOST_TEST(!capture.waits_on("Jump", 0));
	BOOST_TEST(!capture.waits_on("Fire", 1));
	BOOST_TEST(capture.action() == "Jump");
	BOOST_TEST(capture.slot() == 1u);

	capture.begin("Fire", 0);
	BOOST_TEST(capture.waits_on("Fire", 0));
	capture.cancel();
	BOOST_TEST(!capture.active());
	BOOST_TEST(!capture.waits_on("Fire", 0));
}

BOOST_AUTO_TEST_CASE(AnAnswer_IsTold_OnlyWhenThereIsSomethingToTell)
{
	const inp::action_binding key = KB::K;
	gs::rebind_outcome plain;
	BOOST_TEST(view::outcome_text(plain, "Jump", &key).empty());

	gs::rebind_outcome shared;
	shared.result.conflict_with = "Fire";
	BOOST_TEST(view::outcome_text(shared, "Jump", &key) == "K is also used by 'Fire': both keep it");

	gs::rebind_outcome unsaved;
	unsaved.saved = false;
	BOOST_TEST(view::outcome_text(unsaved, "Jump", &key) == "Not saved: the change holds for this run only");
	unsaved.result.conflict_with = "Fire";
	BOOST_TEST(view::outcome_text(unsaved, "Jump", &key)
		== "K is also used by 'Fire': both keep it. Not saved: the change holds for this run only");

	gs::rebind_outcome duplicate;
	duplicate.result.status = inp::rebind_status::duplicate;
	BOOST_TEST(view::outcome_text(duplicate, "Jump", &key) == "'Jump' already has K in another slot");
	gs::rebind_outcome gone;
	gone.result.status = inp::rebind_status::unknown_action;
	BOOST_TEST(view::outcome_text(gone, "Jump", nullptr) == "'Jump' is not an action of the project any more");
}

BOOST_AUTO_TEST_SUITE_END()
