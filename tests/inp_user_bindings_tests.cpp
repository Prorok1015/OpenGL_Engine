#include <boost/test/unit_test.hpp>
#include <snk/input/inp_user_bindings.h>
#include <snk/input/inp_events.hpp>
#include <variant>

// The player's bindings over the project's action map (#736).

namespace {

using KB = inp::KEYBOARD_BUTTONS;
using MB = inp::MOUSE_BUTTONS;

inp::action_map project_map()
{
	inp::action_map map;
	map.add("MoveLeft", { KB::A, KB::LEFT });
	map.add("MoveRight", { KB::D });
	map.add("Fire", { MB::LEFT });
	return map;
}

}

BOOST_AUTO_TEST_SUITE(InpUserBindingsTests)

BOOST_AUTO_TEST_CASE(NoChanges_GiveTheProjectMapWithTheSameIds)
{
	const inp::action_map project = project_map();
	const inp::action_map effective = inp::user_bindings{}.apply(project);
	BOOST_REQUIRE_EQUAL(effective.size(), 3u);
	for (inp::action_id id = 0; id < 3; ++id) {
		BOOST_TEST(effective.find(id)->name == project.find(id)->name);
		BOOST_TEST((effective.find(id)->bindings == project.find(id)->bindings));
	}
}

BOOST_AUTO_TEST_CASE(ARebind_ReplacesTheSlot_AndKeepsTheOtherActionsAndTheIds)
{
	const inp::action_map project = project_map();
	inp::user_bindings user;
	const auto result = user.rebind(project, "MoveRight", 0, KB::E);
	BOOST_REQUIRE(result.ok());
	const inp::action_map effective = user.apply(project);
	BOOST_TEST(effective.id("MoveRight") == project.id("MoveRight"));
	BOOST_TEST((effective.find(effective.id("MoveRight"))->bindings == std::vector<inp::action_binding>{ KB::E }));
	BOOST_TEST((effective.find(effective.id("MoveLeft"))->bindings == project.find(0)->bindings));
}

BOOST_AUTO_TEST_CASE(ARebindWithTheSlotPastTheEnd_AddsABinding_AndFurtherThanThatIsRefused)
{
	const inp::action_map project = project_map();
	inp::user_bindings user;
	BOOST_TEST(user.rebind(project, "MoveRight", 1, KB::RIGHT).ok());
	BOOST_TEST((user.overrides().at("MoveRight") == std::vector<inp::action_binding>{ KB::D, KB::RIGHT }));
	const auto far = user.rebind(project, "MoveRight", 3, KB::E);
	BOOST_TEST((far.status == inp::rebind_status::bad_slot));
	BOOST_TEST(user.overrides().at("MoveRight").size() == 2u);
}

BOOST_AUTO_TEST_CASE(AKeyAnotherActionHas_IsAConflictThatNamesIt_AndChangesNothing)
{
	const inp::action_map project = project_map();
	inp::user_bindings user;
	const auto result = user.rebind(project, "MoveRight", 0, KB::LEFT);
	BOOST_TEST((result.status == inp::rebind_status::conflict));
	BOOST_TEST(result.conflict_with == "MoveLeft");
	BOOST_TEST(user.empty());
}

BOOST_AUTO_TEST_CASE(AConflictIsSeenAgainstTheChangedBindings_NotTheProjectOnes)
{
	const inp::action_map project = project_map();
	inp::user_bindings user;
	BOOST_REQUIRE(user.rebind(project, "MoveLeft", 1, KB::Q).ok());
	// LEFT is free now: MoveLeft gave it up. Q is taken.
	BOOST_TEST(user.rebind(project, "MoveRight", 0, KB::LEFT).ok());
	const auto taken = user.rebind(project, "Fire", 0, KB::Q);
	BOOST_TEST((taken.status == inp::rebind_status::conflict));
	BOOST_TEST(taken.conflict_with == "MoveLeft");
}

BOOST_AUTO_TEST_CASE(TheSameBindingOnTheSameAction_IsNoConflict)
{
	const inp::action_map project = project_map();
	inp::user_bindings user;
	BOOST_TEST(user.rebind(project, "MoveLeft", 1, KB::LEFT).ok());
}

BOOST_AUTO_TEST_CASE(AnUnknownAction_IsRefused)
{
	const inp::action_map project = project_map();
	inp::user_bindings user;
	BOOST_TEST((user.rebind(project, "Dance", 0, KB::E).status == inp::rebind_status::unknown_action));
}

BOOST_AUTO_TEST_CASE(AMouseButtonCanBeBound_AndResetGivesTheProjectBack)
{
	const inp::action_map project = project_map();
	inp::user_bindings user;
	BOOST_REQUIRE(user.rebind(project, "Fire", 0, MB::RIGHT).ok());
	BOOST_TEST((user.apply(project).find(2)->bindings == std::vector<inp::action_binding>{ MB::RIGHT }));
	BOOST_TEST(user.reset("Fire"));
	BOOST_TEST(!user.reset("Fire"));
	BOOST_TEST((user.apply(project).find(2)->bindings == project.find(2)->bindings));

	BOOST_REQUIRE(user.rebind(project, "Fire", 0, MB::RIGHT).ok());
	BOOST_REQUIRE(user.rebind(project, "MoveRight", 0, KB::E).ok());
	user.reset_all();
	BOOST_TEST(user.empty());
}

BOOST_AUTO_TEST_CASE(AChangeOfAnActionTheProjectDoesNotHave_IsIgnoredByApply)
{
	const inp::action_map project = project_map();
	inp::user_bindings user;
	user.set("Gone", { KB::E });
	const inp::action_map effective = user.apply(project);
	BOOST_TEST(effective.size() == 3u);
	BOOST_TEST((effective.find(effective.id("MoveLeft"))->bindings == project.find(0)->bindings));
}

BOOST_AUTO_TEST_SUITE_END()
