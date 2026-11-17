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

BOOST_AUTO_TEST_CASE(TheSameKeyInTwoSlotsOfOneAction_IsRefused)
{
	const inp::action_map project = project_map();
	inp::user_bindings user;
	const auto result = user.rebind(project, "MoveLeft", 0, KB::LEFT);
	BOOST_TEST((result.status == inp::rebind_status::duplicate));
	BOOST_TEST(user.empty());
}

BOOST_AUTO_TEST_CASE(ASharedBinding_IsFoundAndNamedOnce_AndAMapWithoutOneHasNone)
{
	BOOST_TEST(inp::find_binding_conflicts(project_map()).empty());
	inp::action_map shared;
	shared.add("MoveLeft", { KB::A });
	shared.add("MoveRight", { KB::A, KB::D });
	const auto found = inp::find_binding_conflicts(shared);
	BOOST_REQUIRE_EQUAL(found.size(), 1u);
	BOOST_TEST(found[0].first == "MoveLeft");
	BOOST_TEST(found[0].second == "MoveRight");
	BOOST_TEST((found[0].binding == inp::action_binding{ KB::A }));
}

BOOST_AUTO_TEST_CASE(AGamepadButtonAndAnAxisCanBeBound_AndConflictLikeKeys)
{
	inp::action_map project = project_map();
	project.add("Jump", { inp::GAMEPAD_BUTTONS::A });
	inp::user_bindings user;
	BOOST_TEST(user.rebind(project, "Fire", 1, inp::GAMEPAD_AXES::RIGHT_TRIGGER).ok());
	const auto taken = user.rebind(project, "MoveLeft", 0, inp::GAMEPAD_BUTTONS::A);
	BOOST_TEST((taken.status == inp::rebind_status::conflict));
	BOOST_TEST(taken.conflict_with == "Jump");
	BOOST_TEST((user.apply(project).find(project.id("Fire"))->bindings == std::vector<inp::action_binding>{ MB::LEFT, inp::GAMEPAD_AXES::RIGHT_TRIGGER }));
}

BOOST_AUTO_TEST_CASE(TheSameInputWithAnotherThreshold_IsAConflictAndADuplicate)
{
	inp::action_map project = project_map();
	project.add("Aim", { inp::action_binding{ inp::GAMEPAD_AXES::RIGHT_TRIGGER, 0.3f } });
	inp::user_bindings user;
	const auto taken = user.rebind(project, "Fire", 1, inp::GAMEPAD_AXES::RIGHT_TRIGGER);
	BOOST_TEST((taken.status == inp::rebind_status::conflict));
	BOOST_TEST(taken.conflict_with == "Aim");
	BOOST_TEST((user.rebind(project, "Aim", 1, inp::action_binding{ inp::GAMEPAD_AXES::RIGHT_TRIGGER, 0.6f }).status == inp::rebind_status::duplicate));

	inp::action_map shared;
	shared.add("Fire", { inp::GAMEPAD_AXES::RIGHT_TRIGGER });
	shared.add("Aim", { inp::action_binding{ inp::GAMEPAD_AXES::RIGHT_TRIGGER, 0.3f } });
	const auto found = inp::find_binding_conflicts(shared);
	BOOST_REQUIRE_EQUAL(found.size(), 1u);
	BOOST_TEST((found == inp::find_binding_conflicts(shared)));
	BOOST_TEST(found[0].second == "Aim");
}

BOOST_AUTO_TEST_CASE(AConflictAllowed_IsAppliedAndNamed_AndASlotCanBeTakenOut)
{
	const inp::action_map project = project_map();
	inp::user_bindings user;
	const auto result = user.rebind(project, "MoveRight", 0, KB::LEFT, true);
	BOOST_TEST(result.ok());
	BOOST_TEST(result.conflicted());
	BOOST_TEST(result.conflict_with == "MoveLeft");
	const inp::action_map effective = user.apply(project);
	const auto shared = inp::find_binding_conflicts(effective);
	BOOST_REQUIRE_EQUAL(shared.size(), 1u);

	// A duplicate in the same action is refused even when conflicts are allowed.
	BOOST_TEST((user.rebind(project, "MoveLeft", 0, KB::LEFT, true).status == inp::rebind_status::duplicate));

	BOOST_TEST(user.remove(project, "MoveRight", 0).ok());
	BOOST_TEST(user.apply(project).find(project.id("MoveRight"))->bindings.empty());
	BOOST_TEST((user.remove(project, "MoveRight", 0).status == inp::rebind_status::bad_slot));
	BOOST_TEST((user.remove(project, "Dance", 0).status == inp::rebind_status::unknown_action));
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
