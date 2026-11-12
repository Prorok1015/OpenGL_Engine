#include <boost/test/unit_test.hpp>
#include <snk/input/inp_action_state.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_system.h>
#include <entt/entt.hpp>
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
		state.update(ref, keys, bound, nullptr, &fixed, use_reach ? &reach : nullptr, &injection);
		injection.edges.clear();
	}

	// For the next frame only, as gs::route_input hands it over.
	void inject(inp::action_id id, inp::action_edge edge) { injection.edges.push_back({ id, edge }); }

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

	// The key reaches a layer that declares the action; `armed`: the memory of fronts of that layer lets it give a press.
	void reach_key(inp::action_id id, KB key, bool armed)
	{
		reach.reach[id].keys.set(static_cast<std::size_t>(key));
		reach.reach[id].armed_keys.set(static_cast<std::size_t>(key), armed);
	}

	// What reached the layers that declare the actions (#624); not given, the state alone decides.
	bool use_reach = false;
	inp::action_input reach{};
	inp::action_injection injection{};
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

BOOST_AUTO_TEST_CASE(AGamepadBinding_IsNotHeldByAMouseButtonOfTheSameValue)
{
	// GAMEPAD_BUTTONS::A and GAMEPAD_AXES::LEFT_X_NEG are 0 and B and LEFT_X_POS are 1, as MB::LEFT and MB::RIGHT are.
	auto map = std::make_shared<inp::action_map>();
	map->add("PadA", { inp::GAMEPAD_BUTTONS::A });
	map->add("PadB", { inp::GAMEPAD_BUTTONS::B });
	map->add("PadLeft", { inp::GAMEPAD_AXES::LEFT_X_NEG });
	map->add("PadRight", { inp::GAMEPAD_AXES::LEFT_X_POS });
	ref = inp::action_map_ref{ map };
	use_reach = true;
	reach.reach.resize(4);
	for (inp::action_id id = 0; id < 4; ++id) {
		reach.reach[id].buttons = true;
		reach.reach[id].armed_buttons.set();
	}
	set(MB::LEFT, true);
	set(MB::RIGHT, true);
	frame();
	frame();
	for (inp::action_id id = 0; id < 4; ++id) {
		BOOST_TEST_INFO("action " << id);
		BOOST_TEST(!state.held(id));
		BOOST_TEST(edge_free(id));
	}
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

	// The key reaches a layer while it is still held and is not armed there (it was taken when it went down): no press.
	reach_key(JUMP, KB::SPACE, false);
	frame();
	BOOST_TEST(edge_free(JUMP));
	BOOST_TEST(!state.held(JUMP));

	set(KB::SPACE, false);
	frame();
	reach_key(JUMP, KB::SPACE, true);
	set(KB::SPACE, true);
	frame();
	BOOST_TEST(state.pressed(JUMP));

	// A layer above takes the key: the action is let go, once.
	reach.reach[JUMP] = inp::action_reach{};
	frame();
	BOOST_TEST(state.released(JUMP));
	frame();
	BOOST_TEST(edge_free(JUMP));
}

BOOST_AUTO_TEST_CASE(WithReach_TheOtherBindingKeepsAHalfTakenActionHeld)
{
	use_reach = true;
	reach.reach.assign(2, inp::action_reach{});
	// Fire is the left button or F: F reached and is armed, the button did not reach.
	reach_key(FIRE, KB::F, true);
	frame();

	set(KB::F, true);
	frame();
	BOOST_TEST(state.pressed(FIRE));

	// The button goes down but is taken: F still holds the action, no edge.
	set(MB::LEFT, true);
	frame();
	BOOST_TEST(state.held(FIRE));
	BOOST_TEST(edge_free(FIRE));

	// F is let go: the button is down and taken, so the action goes.
	set(KB::F, false);
	frame();
	BOOST_TEST(state.released(FIRE));
	BOOST_TEST(!state.held(FIRE));
}

BOOST_AUTO_TEST_CASE(WithReach_OneBindingHeldAndTakenForGood_DoesNotStopThePressOfTheOther)
{
	use_reach = true;
	reach.reach.assign(2, inp::action_reach{});
	// Fire: the button reached and armed, F is held and taken the whole time (not reached, not armed).
	reach.reach[FIRE].buttons = true;
	reach.reach[FIRE].armed_buttons.set(static_cast<std::size_t>(MB::LEFT));
	frame();
	set(KB::F, true);
	frame();
	frame();
	BOOST_TEST(edge_free(FIRE));

	set(MB::LEFT, true);
	frame();
	BOOST_TEST(state.pressed(FIRE));
	set(MB::LEFT, false);
	frame();
	BOOST_TEST(state.released(FIRE));
}

BOOST_AUTO_TEST_CASE(WithReach_AKeyHeldWhileTheActionDidNotReach_IsNoPressWhenItReachesAgain)
{
	use_reach = true;
	reach.reach.assign(2, inp::action_reach{});
	reach_key(JUMP, KB::SPACE, true);
	frame();

	// A modal layer takes the key before it is pressed and gives it back while it is still held.
	reach.reach[JUMP] = inp::action_reach{};
	frame();
	set(KB::SPACE, true);
	frame();
	reach_key(JUMP, KB::SPACE, false);
	frame();
	BOOST_TEST(edge_free(JUMP));
	BOOST_TEST(!state.held(JUMP));

	// Let go (the memory arms it again), then pressed again: a press.
	set(KB::SPACE, false);
	frame();
	reach_key(JUMP, KB::SPACE, true);
	set(KB::SPACE, true);
	frame();
	BOOST_TEST(state.pressed(JUMP));
}

BOOST_AUTO_TEST_CASE(WithReach_TheCursorOverAnotherWorldWithoutAButton_DoesNotDisarmTheClickOfTheNextFrame)
{
	use_reach = true;
	reach.reach.assign(2, inp::action_reach{});
	frame();
	// The cursor is over the HUD of another world: Fire does not reach, no button is down.
	frame();
	frame();
	// The cursor leaves and the click lands in the same frame (the button was up, so it is armed).
	reach.reach[FIRE].buttons = true;
	reach.reach[FIRE].armed_buttons.set(static_cast<std::size_t>(MB::LEFT));
	set(MB::LEFT, true);
	frame();
	BOOST_TEST(state.pressed(FIRE));
}

BOOST_AUTO_TEST_CASE(WithReach_AKeyPressedInTheFrameTheModalLayerCloses_IsAPress)
{
	use_reach = true;
	reach.reach.assign(2, inp::action_reach{});
	frame();
	frame();
	reach_key(JUMP, KB::SPACE, true);
	set(KB::SPACE, true);
	frame();
	BOOST_TEST(state.pressed(JUMP));
}

BOOST_AUTO_TEST_CASE(WithReach_ABindingDownWhenTheMapComesIn_GivesNoPress_ButAnotherBindingDoes)
{
	use_reach = true;
	reach.reach.assign(2, inp::action_reach{});
	reach_key(FIRE, KB::F, true);
	reach.reach[FIRE].buttons = true;
	reach.reach[FIRE].armed_buttons.set(static_cast<std::size_t>(MB::LEFT));
	frame();
	set(KB::F, true);
	frame();
	BOOST_TEST(state.pressed(FIRE));

	// A new map while F is held: F gives no press until it is let go (the memory of fronts says it is armed, the new
	// state does not trust that), the left button is another binding and does.
	ref.map = make_map();
	frame();
	frame();
	BOOST_TEST(!state.held(FIRE));
	BOOST_TEST(edge_free(FIRE));
	set(MB::LEFT, true);
	frame();
	BOOST_TEST(state.pressed(FIRE));
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

// ---------------------------------------------------------------- inject (#624)

BOOST_AUTO_TEST_CASE(Inject_APressHoldsTheActionWithoutAnyKey_UntilAReleaseComes)
{
	frame();
	inject(JUMP, inp::action_edge::pressed);
	frame();
	BOOST_TEST(state.pressed(JUMP));
	BOOST_TEST(state.held(JUMP));
	BOOST_TEST(step().pressed);

	frame();
	BOOST_TEST(state.held(JUMP));
	BOOST_TEST(edge_free(JUMP));

	inject(JUMP, inp::action_edge::released);
	frame();
	BOOST_TEST(state.released(JUMP));
	BOOST_TEST(!state.held(JUMP));
	BOOST_TEST(step().released);

	frame();
	BOOST_TEST(edge_free(JUMP));
}

BOOST_AUTO_TEST_CASE(Inject_APressOfAnActionAlreadyHeldByAKey_IsNoSecondPress)
{
	frame();
	set(KB::SPACE, true);
	frame();
	BOOST_TEST(state.pressed(JUMP));

	inject(JUMP, inp::action_edge::pressed);
	frame();
	BOOST_TEST(edge_free(JUMP));

	set(KB::SPACE, false);
	frame();
	BOOST_TEST(state.held(JUMP));
	BOOST_TEST(edge_free(JUMP));
}

BOOST_AUTO_TEST_CASE(Inject_TheKeysComeAndGoWhileInjectedHeld_ThenTheReleaseLetsGo)
{
	frame();
	inject(JUMP, inp::action_edge::pressed);
	frame();
	set(KB::SPACE, true);
	frame();
	set(KB::SPACE, false);
	frame();
	BOOST_TEST(state.held(JUMP));
	BOOST_TEST(edge_free(JUMP));

	inject(JUMP, inp::action_edge::released);
	frame();
	BOOST_TEST(state.released(JUMP));
	BOOST_TEST(!state.held(JUMP));
}

BOOST_AUTO_TEST_CASE(Inject_AReleaseWhileAKeyHoldsTheAction_KeepsItHeldUntilTheKeyGoes)
{
	frame();
	inject(JUMP, inp::action_edge::pressed);
	frame();
	set(KB::SPACE, true);
	frame();

	inject(JUMP, inp::action_edge::released);
	frame();
	BOOST_TEST(state.held(JUMP));
	BOOST_TEST(edge_free(JUMP));

	set(KB::SPACE, false);
	frame();
	BOOST_TEST(state.released(JUMP));
	BOOST_TEST(!state.held(JUMP));
}

BOOST_AUTO_TEST_CASE(Inject_APressAndAReleaseInOneFrame_GiveBothEdgesAndTheFixedStepsSeeThemInOrder)
{
	frame();
	inject(JUMP, inp::action_edge::pressed);
	inject(JUMP, inp::action_edge::released);
	frame();
	BOOST_TEST(state.pressed(JUMP));
	BOOST_TEST(state.released(JUMP));
	BOOST_TEST(!state.held(JUMP));

	const auto first = step();
	BOOST_TEST(first.pressed);
	const auto second = step();
	BOOST_TEST(second.released);
}

BOOST_AUTO_TEST_CASE(Inject_LosingTheBindingLetsTheKeysGoButNotTheInjectedHold)
{
	frame();
	inject(JUMP, inp::action_edge::pressed);
	inject(FIRE, inp::action_edge::pressed);
	frame();
	set(KB::F, true);
	frame();

	bound = { .bound = false, .frozen = false };
	frame();
	BOOST_TEST(state.held(JUMP));
	BOOST_TEST(state.held(FIRE));
	BOOST_TEST(edge_free(JUMP));
}

BOOST_AUTO_TEST_CASE(Inject_AFrozenWorldLosesTheEdges)
{
	frame();
	bound = { .bound = false, .frozen = true };
	inject(JUMP, inp::action_edge::pressed);
	frame();
	BOOST_TEST(!state.held(JUMP));

	bound = { .bound = true, .frozen = false };
	frame();
	BOOST_TEST(!state.held(JUMP));
}

BOOST_AUTO_TEST_CASE(Inject_ANewMapLetsTheInjectedHoldGoWithOneReleased_AndTakesAnEdgeOfThatFrame)
{
	frame();
	inject(JUMP, inp::action_edge::pressed);
	frame();
	BOOST_TEST(state.held(JUMP));

	ref.map = make_map();
	inject(FIRE, inp::action_edge::pressed);
	frame();
	BOOST_TEST(!state.held(JUMP));
	BOOST_TEST(state.held(FIRE));
	BOOST_TEST(state.pressed(FIRE));

	set(KB::F, false);
	frame();
	BOOST_TEST(state.held(FIRE));
}

BOOST_AUTO_TEST_CASE(Inject_AReleaseIsNotHeldBackByAKeyThatGivesNoPress)
{
	use_reach = true;
	reach.reach.resize(2);
	// Space is held when the world gets the input back: it reaches the layer but is not armed, so it never pressed Jump.
	set(KB::SPACE, true);
	reach_key(JUMP, KB::SPACE, false);
	frame();
	BOOST_TEST(!state.held(JUMP));

	inject(JUMP, inp::action_edge::pressed);
	frame();
	BOOST_TEST(state.held(JUMP));
	inject(JUMP, inp::action_edge::released);
	frame();
	BOOST_TEST(state.released(JUMP));
	BOOST_TEST(!state.held(JUMP));

	frame();
	BOOST_TEST(!state.held(JUMP));
	BOOST_TEST(edge_free(JUMP));
}

BOOST_AUTO_TEST_CASE(Inject_AReleaseIsHeldBackByAnArmedKeyThatHoldsTheAction)
{
	use_reach = true;
	reach.reach.resize(2);
	frame();
	inject(JUMP, inp::action_edge::pressed);
	frame();
	set(KB::SPACE, true);
	reach_key(JUMP, KB::SPACE, true);
	frame();
	inject(JUMP, inp::action_edge::released);
	frame();
	BOOST_TEST(state.held(JUMP));
	BOOST_TEST(edge_free(JUMP));
}

BOOST_AUTO_TEST_CASE(Inject_AnIdOutsideTheMapIsIgnored)
{
	frame();
	inject(inp::action_id{ 7 }, inp::action_edge::pressed);
	frame();
	BOOST_TEST(edge_free(JUMP));
	BOOST_TEST(edge_free(FIRE));
}

BOOST_AUTO_TEST_CASE(TheTraceKeepsTheUpdateOfEachActionsLastEdge)
{
	// The system as the engine registers it: its edges go to the world's bus (#605).
	ecs::bus_registry types;
	types.add<inp::action_event>({}, &inp::action_event_json);
	entt::registry reg;
	auto& bus = reg.ctx().emplace<ecs::world_bus>();
	BOOST_REQUIRE(bus.bind(types, reg, "w"));
	auto& events = reg.ctx().get<ecs::event_writer<inp::action_event>>();
	inp::action_trace trace;
	const auto run = [&] { inp::update_and_trace_actions_system(events, keys, bound, ref, reach, injection, state, fixed, trace, bus); };

	run();
	BOOST_TEST(trace.updates == 1u);
	BOOST_TEST(trace.map_since_delivery == 1u); // the first map: this frame's delivery
	BOOST_REQUIRE(trace.last_edges.size() == 2u);
	BOOST_TEST(!trace.last_edges[JUMP].has_value());

	set(KB::SPACE, true);
	run();
	BOOST_REQUIRE(trace.last_edges[JUMP].has_value());
	BOOST_TEST(trace.last_edges[JUMP]->update == 2u);
	BOOST_TEST((trace.last_edges[JUMP]->kind == inp::action_edge::pressed));

	run();
	BOOST_TEST(trace.last_edges[JUMP]->update == 2u); // held: no new edge

	set(KB::SPACE, false);
	run();
	BOOST_TEST(trace.last_edges[JUMP]->update == 4u);
	BOOST_TEST((trace.last_edges[JUMP]->kind == inp::action_edge::released));
	BOOST_TEST(!trace.last_edges[FIRE].has_value());

	// Another map starts the trace again, from the delivery after the one carrying the old map's releases.
	bus.deliver();
	ref.map = make_map();
	run();
	BOOST_TEST((trace.map == ref.map));
	BOOST_TEST(trace.map_since_delivery == 3u);
	BOOST_TEST(!trace.last_edges[JUMP].has_value());
	BOOST_TEST(inp::action_event_json({ JUMP, inp::action_edge::pressed }) == R"json({"action":0,"edge":"pressed"})json");
}

BOOST_AUTO_TEST_SUITE_END()
