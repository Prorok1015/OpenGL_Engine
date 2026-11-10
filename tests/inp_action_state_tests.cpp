#include <boost/test/unit_test.hpp>
#include <snk/input/inp_action_state.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/ecs/ecs_system.h>
#include <memory>

// The state of the game actions in one world (#604 PR 2, WORLD_BUS.md 4.2), frame by frame, without a scene: the rule
// "any binding holds the action", the armed rule, losing the binding, a frozen Step, a new map.

namespace {

using KB = inp::KEYBOARD_BUTTONS;
using MB = inp::MOUSE_BUTTONS;

constexpr inp::action_id JUMP = 0;
constexpr inp::action_id FIRE = 1;

std::shared_ptr<const inp::action_map> make_map()
{
	auto map = std::make_shared<inp::action_map>();
	map->add("Jump", { KB::SPACE });
	map->add("Fire", { MB::LEFT, KB::F });
	return map;
}

struct actions_fixture
{
	void frame()
	{
		state.update(ref, keys, bound, nullptr, &fixed, use_reach ? &reach : nullptr);
	}

	// One fixed step; what it sees of Jump.
	struct seen_step
	{
		bool pressed = false;
		bool released = false;
		bool held = false;
	};
	seen_step step()
	{
		fixed.begin_step();
		return { fixed.pressed(JUMP), fixed.released(JUMP), fixed.held(JUMP) };
	}

	void set(KB key, bool down) { keys.keyboard[static_cast<std::size_t>(key)] = down; }
	void set(MB button, bool down) { keys.mouse[static_cast<std::size_t>(button)] = down; }

	bool edge_free(inp::action_id id) const { return !state.pressed(id) && !state.released(id); }

	// What reached the layers that declare the actions (#624); not given, the state alone decides.
	bool use_reach = false;
	inp::action_input reach{};
	inp::action_map_ref ref{ make_map() };
	inp::input_state keys{};
	inp::input_bound bound{ .bound = true, .frozen = false };
	inp::action_state state{};
	inp::fixed_action_state fixed{};
};

}

BOOST_FIXTURE_TEST_SUITE(InpActionStateTests, actions_fixture)

BOOST_AUTO_TEST_CASE(APressAndARelease_GiveOneEdgeEach)
{
	frame();
	BOOST_TEST(edge_free(JUMP));

	set(KB::SPACE, true);
	frame();
	BOOST_TEST(state.pressed(JUMP));
	BOOST_TEST(state.held(JUMP));

	frame();
	BOOST_TEST(state.held(JUMP));
	BOOST_TEST(edge_free(JUMP));

	set(KB::SPACE, false);
	frame();
	BOOST_TEST(state.released(JUMP));
	BOOST_TEST(!state.held(JUMP));

	frame();
	BOOST_TEST(edge_free(JUMP));
}

BOOST_AUTO_TEST_CASE(AnyBindingHoldsTheAction)
{
	frame();
	set(MB::LEFT, true);
	frame();
	BOOST_TEST(state.pressed(FIRE));

	set(KB::F, true);
	frame();
	BOOST_TEST(edge_free(FIRE));

	set(MB::LEFT, false);
	frame();
	BOOST_TEST(state.held(FIRE));
	BOOST_TEST(edge_free(FIRE));

	set(KB::F, false);
	frame();
	BOOST_TEST(state.released(FIRE));
}

BOOST_AUTO_TEST_CASE(ComingBackWithTheKeyHeld_GivesNoPressUntilItIsLetGo)
{
	set(KB::SPACE, true);
	frame();
	frame();
	BOOST_TEST(!state.held(JUMP));
	BOOST_TEST(edge_free(JUMP));

	set(KB::SPACE, false);
	frame();
	BOOST_TEST(edge_free(JUMP));

	set(KB::SPACE, true);
	frame();
	BOOST_TEST(state.pressed(JUMP));
}

BOOST_AUTO_TEST_CASE(LosingTheBinding_LetsEveryHeldActionGoOnce_AndDisarmsIt)
{
	frame();
	set(KB::SPACE, true);
	frame();
	BOOST_REQUIRE(state.held(JUMP));

	bound.bound = false;
	frame();
	BOOST_TEST(state.released(JUMP));
	BOOST_TEST(!state.held(JUMP));
	frame();
	BOOST_TEST(edge_free(JUMP));

	bound.bound = true;
	frame();
	BOOST_TEST(edge_free(JUMP));
	BOOST_TEST(!state.held(JUMP));
}

BOOST_AUTO_TEST_CASE(AFrozenStep_KeepsTheStateWithoutEdges)
{
	frame();
	set(KB::SPACE, true);
	frame();
	BOOST_REQUIRE(state.pressed(JUMP));

	bound = { .bound = false, .frozen = true };
	set(KB::SPACE, false);
	set(MB::LEFT, true);
	frame();
	BOOST_TEST(state.held(JUMP));
	BOOST_TEST(edge_free(JUMP));
	BOOST_TEST(!state.held(FIRE));
	BOOST_TEST(edge_free(FIRE));
}

BOOST_AUTO_TEST_CASE(ANewMap_StartsFromTheKeysWithoutEdges)
{
	frame();
	set(KB::SPACE, true);
	frame();
	BOOST_REQUIRE(state.held(JUMP));

	ref.map = make_map();
	frame();
	BOOST_TEST(edge_free(JUMP));
	set(KB::SPACE, false);
	frame();
	BOOST_TEST(edge_free(JUMP));
	set(KB::SPACE, true);
	frame();
	BOOST_TEST(state.pressed(JUMP));
}

BOOST_AUTO_TEST_CASE(FramesThatDoNotRun_AreSeenOnTheNextOne)
{
	// Pause: the system does not run, so the next frame compares with the one before the pause.
	frame();
	set(KB::SPACE, true);
	frame();
	set(KB::SPACE, false);
	frame();
	BOOST_TEST(state.released(JUMP));
	BOOST_TEST(!state.pressed(JUMP));
}

BOOST_AUTO_TEST_CASE(ByNameIsByIdAndAnUnknownNameIsFalse)
{
	frame();
	set(KB::SPACE, true);
	frame();
	BOOST_TEST(state.pressed("Jump"));
	BOOST_TEST(state.held("Jump"));
	BOOST_TEST(!state.pressed("Fire"));
	BOOST_TEST(!state.held("Dash"));
	BOOST_TEST(!state.held(inp::invalid_action));
	BOOST_TEST(!state.held(inp::action_id{ 7 }));
}

BOOST_AUTO_TEST_CASE(NoMap_NoActions)
{
	ref.map = nullptr;
	set(KB::SPACE, true);
	frame();
	frame();
	BOOST_TEST(!state.held(JUMP));
	BOOST_TEST(!state.held("Jump"));
}

BOOST_AUTO_TEST_CASE(TheInputSystemsAreRegisteredTogether)
{
	// null_engine_fixture (and input_init) register through this function, so their worlds can list update_actions.
	ecs::system_factory factory;
	inp::register_ecs_input_system(factory, std::make_shared<inp::ecs_input_manager>());
	BOOST_TEST(factory.registry().find("inp::update_input_state") != nullptr);
	const auto* actions = factory.registry().find("inp::update_actions");
	BOOST_REQUIRE(actions != nullptr);
	BOOST_TEST((actions->desc.phase == ecs::phase::input));
	BOOST_TEST(!actions->desc.run_in_edit);
}

// ---------------------------------------------------------------- fixed_action_state

BOOST_AUTO_TEST_CASE(Fixed_AFrameWithoutSteps_KeepsTheEdgeForTheNextStep)
{
	frame();
	set(KB::SPACE, true);
	frame();
	frame(); // no step in these frames (time_scale 0, a short frame, Pause)
	const auto first = step();
	BOOST_TEST(first.pressed);
	BOOST_TEST(first.held);
	const auto second = step();
	BOOST_TEST(!second.pressed);
	BOOST_TEST(!second.released);
}

BOOST_AUTO_TEST_CASE(Fixed_SeveralStepsInAFrame_SeeThePressOnce_AndTheLevelInEach)
{
	frame();
	set(KB::SPACE, true);
	frame();
	int presses = 0;
	for (int i = 0; i < 3; ++i) {
		const auto s = step();
		presses += s.pressed ? 1 : 0;
		BOOST_TEST(s.held);
	}
	BOOST_TEST(presses == 1);
}

BOOST_AUTO_TEST_CASE(Fixed_APressLetGoBeforeAnyStep_IsAStepWithPressedThenOneWithReleased)
{
	frame();
	set(KB::SPACE, true);
	frame();
	set(KB::SPACE, false);
	frame();
	frame();
	const auto first = step();
	BOOST_TEST(first.pressed);
	BOOST_TEST(!first.held); // the frame's level
	const auto second = step();
	BOOST_TEST(second.released);
	BOOST_TEST(!second.pressed);
	const auto third = step();
	BOOST_TEST(!third.pressed);
	BOOST_TEST(!third.released);
}

BOOST_AUTO_TEST_CASE(Fixed_AFullQueue_DropsTheOldestPairs_SoTheStepsStayBalanced)
{
	frame();
	// Ten edges with no step: press, release, ... The queue holds eight.
	for (int i = 0; i < 5; ++i) {
		set(KB::SPACE, true);
		frame();
		set(KB::SPACE, false);
		frame();
	}
	int presses = 0;
	int releases = 0;
	bool expect_press = true;
	for (std::size_t i = 0; i < inp::fixed_action_state::queue_limit + 2; ++i) {
		const auto s = step();
		if (!s.pressed && !s.released) continue;
		BOOST_TEST(s.pressed == expect_press);
		expect_press = !expect_press;
		presses += s.pressed ? 1 : 0;
		releases += s.released ? 1 : 0;
	}
	BOOST_TEST(presses == 4);
	BOOST_TEST(releases == 4);
}

BOOST_AUTO_TEST_CASE(Fixed_AMapChange_GivesTheStepsTheReleaseOfWhatTheySawPressed)
{
	frame();
	set(KB::SPACE, true);
	frame();
	BOOST_REQUIRE(step().pressed);

	ref.map = make_map(); // the key stays held
	frame();
	frame(); // a frame without a step
	const auto first = step();
	BOOST_TEST(first.released);
	BOOST_TEST(!first.pressed);
	const auto second = step();
	BOOST_TEST(!second.released);
	BOOST_TEST(!second.pressed);
}

BOOST_AUTO_TEST_CASE(Fixed_AMapChange_DropsAPressTheStepsNeverSaw)
{
	frame();
	set(KB::SPACE, true);
	frame();
	ref.map = make_map();
	frame();
	const auto s = step();
	BOOST_TEST(!s.pressed);
	BOOST_TEST(!s.released);
}

BOOST_AUTO_TEST_CASE(Fixed_ByNameIsById)
{
	frame();
	set(KB::SPACE, true);
	frame();
	fixed.begin_step();
	BOOST_TEST(fixed.pressed("Jump"));
	BOOST_TEST(fixed.held("Jump"));
	BOOST_TEST(!fixed.pressed("Fire"));
	BOOST_TEST(!fixed.released("Dash"));
}

BOOST_AUTO_TEST_CASE(WithReach_AnActionIsDownOnlyThroughABindingThatReachedALayerOfTheWorld)
{
	use_reach = true;
	reach.reach.assign(2, inp::action_reach{});
	frame();

	set(KB::SPACE, true);
	frame();
	BOOST_TEST(!state.held(JUMP));

	reach.reach[JUMP].keys.set(static_cast<std::size_t>(KB::SPACE));
	frame();
	BOOST_TEST(state.pressed(JUMP));

	// A layer above takes the key: the action is let go, once.
	reach.reach[JUMP].keys.reset();
	frame();
	BOOST_TEST(state.released(JUMP));
	frame();
	BOOST_TEST(edge_free(JUMP));
}

BOOST_AUTO_TEST_CASE(WithReach_TheOtherBindingKeepsAHalfTakenActionHeld)
{
	use_reach = true;
	reach.reach.assign(2, inp::action_reach{});
	// Fire is the left button or F: the button reached, F did not.
	reach.reach[FIRE].buttons = true;
	frame();

	set(KB::F, true);
	frame();
	BOOST_TEST(!state.held(FIRE));

	reach.reach[FIRE].keys.set(static_cast<std::size_t>(KB::F));
	frame();
	BOOST_TEST(state.pressed(FIRE));

	// F is taken again but the button is down and reached: still held, no edge.
	set(MB::LEFT, true);
	reach.reach[FIRE].keys.reset();
	frame();
	BOOST_TEST(state.held(FIRE));
	BOOST_TEST(edge_free(FIRE));

	set(MB::LEFT, false);
	frame();
	BOOST_TEST(state.released(FIRE));
}

BOOST_AUTO_TEST_CASE(WithReach_AKeyHeldWhileTheActionDidNotReach_IsNoPressWhenItReachesAgain)
{
	use_reach = true;
	reach.reach.assign(2, inp::action_reach{});
	const auto space = static_cast<std::size_t>(KB::SPACE);
	reach.reach[JUMP].keys.set(space);
	frame();

	// A modal layer takes the key before it is pressed and gives it back while it is still held.
	reach.reach[JUMP].keys.reset();
	frame();
	set(KB::SPACE, true);
	frame();
	reach.reach[JUMP].keys.set(space);
	frame();
	BOOST_TEST(edge_free(JUMP));
	BOOST_TEST(!state.held(JUMP));

	// Let go, then pressed again: a press.
	set(KB::SPACE, false);
	frame();
	set(KB::SPACE, true);
	frame();
	BOOST_TEST(state.pressed(JUMP));
}

BOOST_AUTO_TEST_CASE(WithReach_AnActionOutsideTheVectorIsNeverDown)
{
	use_reach = true;
	reach.reach.assign(1, inp::action_reach{});
	frame();
	set(MB::LEFT, true);
	frame();
	frame();
	BOOST_TEST(!state.held(FIRE));
}

BOOST_AUTO_TEST_SUITE_END()
