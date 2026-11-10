#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/game/gs_input_actions.h>
#include <snk/game/gs_input_routing.h>
#include <snk/input/inp_action_state.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/input/inp_events.hpp>
#include <snk/input/inp_input_router.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/ecs/ecs_fixed_stepper.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Game actions through the scene (#604 PR 2, WORLD_BUS.md 4.2): gs::route_input tells each world whether it is bound and
// hands the map to one of them, inp::update_actions keeps the state and puts the edges on the world's bus, and
// gs::input_actions follows actions.desc on disk.

namespace {

using core::inp::KEY_ACTION;
using KB = core::inp::KEYBOARD_BUTTONS;

const glm::ivec4 LEFT_RECT{ 0, 0, 199, 200 };

constexpr const char* ACTIONS_SPACE = R"json({"__type":"input_actions_desc","__version":1,"actions":[
	{"name":"Jump","bindings":[{"key":"SPACE"}]}]})json";
constexpr const char* ACTIONS_J = R"json({"__type":"input_actions_desc","__version":1,"actions":[
	{"name":"Jump","bindings":[{"key":"J"}]}]})json";

const std::vector<std::string> WITH_ACTIONS{ "inp::update_input_state", "inp::update_actions" };

struct seen_event
{
	std::string world;
	inp::action_id action = inp::invalid_action;
	inp::action_edge edge = inp::action_edge::pressed;
};

// What the fixed steps of a world saw of Jump.
struct fixed_probe
{
	int steps = 0;
	int pressed = 0;
	int released = 0;
	int first_pressed_step = -1;
};

void fixed_probe_system(const inp::fixed_action_state& actions, fixed_probe& out)
{
	if (actions.pressed(inp::action_id{ 0 })) {
		if (out.first_pressed_step < 0) out.first_pressed_step = out.steps;
		++out.pressed;
	}
	out.released += actions.released(inp::action_id{ 0 }) ? 1 : 0;
	++out.steps;
}

std::shared_ptr<const inp::action_map> jump_on_space()
{
	auto map = std::make_shared<inp::action_map>();
	map->add("Jump", { KB::SPACE });
	return map;
}

struct actions_fixture : test_util::scene_env
{
	actions_fixture()
		: source(std::make_shared<inp::ecs_input_manager>())
		, router(source)
		, hot(std::make_shared<scn::hot_reload_manager>(*assembler, *rs))
		, mgr(*rs, *assembler, sf, hot)
	{
		inp::register_ecs_input_system(sf, source);
		sf.register_system<&fixed_probe_system>("test::fixed_probe", { .phase = ecs::phase::fixed_update });
		mgr.add_event<inp::action_event>();
		router.set_default_target(inp::input_target{ .set = "level" });
		subscription = mgr.subscribe<inp::action_event>({}, [this](ecs::bus_context& ctx, const ecs::envelope<inp::action_event>& e) {
			seen.push_back({ std::string{ ctx.world() }, e.payload.action, e.payload.edge });
		});
	}

	scn::world_context& world(std::string_view name, uint32_t desc_index, const std::vector<std::string>& systems = WITH_ACTIONS)
	{
		if (!mgr.find_set("level")) mgr.create_set("level");
		auto& context = mgr.create_context("level", name, scn::context_kind::level, systems);
		context.set_desc_index(desc_index);
		return context;
	}

	void key(KB k, KEY_ACTION a)
	{
		inp::input_event e;
		e.construct_payload<core::inp::keyboard_event>(core::inp::keyboard_event{ k, a });
		source->on_handle_event(nullptr, e);
	}

	void move_to(glm::vec2 pos)
	{
		inp::input_event e;
		e.construct_payload<core::inp::cursor_move_event>(core::inp::cursor_move_event{ pos });
		source->on_handle_event(nullptr, e);
	}

	void frame(const std::shared_ptr<const inp::action_map>& map, float dt = 0.016f)
	{
		gs::route_input(router, mgr, map);
		mgr.update(std::chrono::duration<float>(dt));
	}

	// A world whose fixed step is 10 ms, with the probe in fixed_update.
	scn::world_context& fixed_world()
	{
		auto& w = world("a", 0, { "inp::update_input_state", "inp::update_actions", "test::fixed_probe" });
		w.set_stepper(ecs::fixed_stepper(0.01));
		return w;
	}
	static const fixed_probe& probe_of(scn::world_context& w) { return w.state().ctx().get<fixed_probe>(); }

	void unbind() { router.set_default_target(inp::input_target{ .set = "nowhere" }); }
	void bind() { router.set_default_target(inp::input_target{ .set = "level" }); }
	void mode(ecs::run_mode m) { mgr.get_set("level").set_mode(m); }

	std::size_t count(inp::action_edge edge) const
	{
		std::size_t n = 0;
		for (const auto& e : seen) n += e.edge == edge ? 1 : 0;
		return n;
	}

	static const inp::input_bound& bound_of(scn::world_context& w) { return w.state().ctx().get<inp::input_bound>(); }
	static const inp::action_map_ref& map_of(scn::world_context& w) { return w.state().ctx().get<inp::action_map_ref>(); }
	// The state exists once inp::update_actions ran in the world.
	static bool held(scn::world_context& w, std::string_view name)
	{
		const auto* state = w.state().ctx().find<inp::action_state>();
		return state && state->held(name);
	}
	static bool pressed(scn::world_context& w, std::string_view name)
	{
		const auto* state = w.state().ctx().find<inp::action_state>();
		return state && state->pressed(name);
	}

	// Loads `tag` through `actions` (the desc loads on the task pump between two updates).
	void load(gs::input_actions& actions, const res::tag& tag)
	{
		actions.update(tag.view());
		pump();
		actions.update(tag.view());
	}

	std::shared_ptr<inp::ecs_input_manager> source;
	inp::input_router router;
	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	scn::context_manager mgr;
	std::vector<seen_event> seen;
	ecs::connection subscription;
};

}

BOOST_FIXTURE_TEST_SUITE(GsInputActionsTests, actions_fixture)

BOOST_AUTO_TEST_CASE(OnlyTheBoundWorldIsMarked_TheFirstInTheFileReadsTheActions)
{
	auto& a = world("a", 0);
	auto& b = world("b", 1);
	unbind();
	router.bind({ "level", "a" }, LEFT_RECT);
	move_to({ 50.f, 50.f });
	const auto map = jump_on_space();
	frame(map);

	BOOST_TEST(bound_of(a).bound);
	BOOST_TEST(!bound_of(a).frozen);
	BOOST_TEST(map_of(a).map == map);
	BOOST_TEST(!bound_of(b).bound);
	BOOST_TEST(!map_of(b).map);

	// Bound or not, the reader keeps the map: it has to let its actions go when the binding moves away.
	router.clear_bindings();
	router.bind({ "level", "b" }, LEFT_RECT);
	frame(map);
	BOOST_TEST(!bound_of(a).bound);
	BOOST_TEST(map_of(a).map == map);
	BOOST_TEST(bound_of(b).bound);
	BOOST_TEST(!map_of(b).map);
}

BOOST_AUTO_TEST_CASE(TwoWorldsWithTheSystem_TheFirstInTheLevelFileGetsTheActions)
{
	// Created out of file order: the id order of the set must not decide.
	auto& second = world("second", 1);
	auto& first = world("first", 0);
	const auto map = jump_on_space();
	frame(map);

	BOOST_TEST(bound_of(first).bound);
	BOOST_TEST(bound_of(second).bound);
	BOOST_TEST(map_of(first).map == map);
	BOOST_TEST(!map_of(second).map);

	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(pressed(first, "Jump"));
	BOOST_TEST(!held(second, "Jump"));
	BOOST_REQUIRE_EQUAL(seen.size(), 1u);
	BOOST_TEST(seen[0].world == "first");
}

BOOST_AUTO_TEST_CASE(TheDefaultReader_TakesTheKeysOfTheActionsFromTheWorldsBelowIt)
{
	auto& first = world("first", 0);
	auto& second = world("second", 1);
	const auto map = jump_on_space();
	frame(map);
	key(KB::SPACE, KEY_ACTION::DOWN);
	key(KB::W, KEY_ACTION::DOWN);
	frame(map);

	const auto space = static_cast<std::size_t>(KB::SPACE);
	const auto w = static_cast<std::size_t>(KB::W);
	BOOST_TEST(first.state().ctx().get<inp::input_state>().keyboard[space]);
	BOOST_TEST(!second.state().ctx().get<inp::input_state>().keyboard[space]);
	// Only the keys of actions: the rest goes to both worlds.
	BOOST_TEST(first.state().ctx().get<inp::input_state>().keyboard[w]);
	BOOST_TEST(second.state().ctx().get<inp::input_state>().keyboard[w]);
}

BOOST_AUTO_TEST_CASE(AWorldThatDeclaresItsActions_ReadsThemAndTheDefaultReaderGoes)
{
	auto& first = world("first", 0);
	auto& second = world("second", 1);
	scn::input_stack_desc declared;
	declared.actions = { "Jump" };
	second.set_input_stack(declared);
	const auto map = jump_on_space();
	frame(map);

	BOOST_TEST(!map_of(first).map);
	BOOST_TEST(map_of(second).map == map);

	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(pressed(second, "Jump"));
	BOOST_TEST(!held(first, "Jump"));
	BOOST_REQUIRE_EQUAL(seen.size(), 1u);
	BOOST_TEST(seen[0].world == "second");
}

BOOST_AUTO_TEST_CASE(AModalWorldAbove_TakesTheKeysOfTheActions_AndGivesThemBackWhenItIsOff)
{
	auto& game = world("game", 0);
	auto& menu = world("menu", 1, { "inp::update_input_state" });
	scn::input_stack_desc modal;
	modal.order = 5;
	modal.block_below = true;
	menu.set_input_stack(modal);
	const auto map = jump_on_space();
	frame(map);

	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(!held(game, "Jump"));
	BOOST_TEST(seen.empty());

	key(KB::SPACE, KEY_ACTION::UP);
	frame(map);
	modal.active = false;
	menu.set_input_stack(modal);
	frame(map);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(pressed(game, "Jump"));
	BOOST_REQUIRE_EQUAL(seen.size(), 1u);
	BOOST_TEST(seen[0].world == "game");
}

BOOST_AUTO_TEST_CASE(AHalfTakenAction_StaysHeldThroughTheOtherKey_AndIsLetGoOnce)
{
	auto& game = world("game", 1);
	auto& ui = world("ui", 0, { "inp::update_input_state" });
	scn::input_stack_desc declared;
	declared.actions = { "MoveLeft" };
	game.set_input_stack(declared);
	scn::input_stack_desc top;
	top.order = 5;
	top.keys = { "A" };
	ui.set_input_stack(top);
	auto map = std::make_shared<inp::action_map>();
	map->add("MoveLeft", { KB::A, KB::LEFT });
	frame(map);

	// A is taken by the world above, the left arrow reaches the game.
	key(KB::A, KEY_ACTION::DOWN);
	key(KB::LEFT, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(pressed(game, "MoveLeft"));
	frame(map);
	BOOST_TEST(held(game, "MoveLeft"));
	BOOST_TEST(count(inp::action_edge::pressed) == 1u);

	key(KB::LEFT, KEY_ACTION::UP);
	frame(map);
	BOOST_TEST(!held(game, "MoveLeft"));
	BOOST_TEST(count(inp::action_edge::released) == 1u);
	frame(map);
	BOOST_TEST(count(inp::action_edge::pressed) == 1u);
	BOOST_TEST(count(inp::action_edge::released) == 1u);
}

BOOST_AUTO_TEST_CASE(AnEditSet_KeepsEveryRawKey_TheActionsComeWithPlay)
{
	auto& first = world("first", 0);
	auto& second = world("second", 1);
	mode(ecs::run_mode::edit);
	const auto map = jump_on_space();
	frame(map);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);

	const auto space = static_cast<std::size_t>(KB::SPACE);
	BOOST_TEST(first.state().ctx().get<inp::input_state>().keyboard[space]);
	BOOST_TEST(second.state().ctx().get<inp::input_state>().keyboard[space]);
	BOOST_TEST(!map_of(first).map);
}

BOOST_AUTO_TEST_CASE(InPlay_ThePressReachesASubscriberInTheSameFrame)
{
	world("a", 0);
	const auto map = jump_on_space();
	frame(map);
	BOOST_TEST(seen.empty());

	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_REQUIRE_EQUAL(seen.size(), 1u);
	BOOST_TEST(seen[0].world == "a");
	BOOST_TEST(seen[0].action == inp::action_id{ 0 });
	BOOST_TEST((seen[0].edge == inp::action_edge::pressed));

	key(KB::SPACE, KEY_ACTION::UP);
	frame(map);
	BOOST_REQUIRE_EQUAL(seen.size(), 2u);
	BOOST_TEST((seen[1].edge == inp::action_edge::released));
}

BOOST_AUTO_TEST_CASE(AKeyLetGoDuringPause_GivesOneReleaseOnPlay_AndNoPress)
{
	auto& a = world("a", 0);
	const auto map = jump_on_space();
	frame(map);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_REQUIRE_EQUAL(count(inp::action_edge::pressed), 1u);

	mode(ecs::run_mode::pause);
	key(KB::SPACE, KEY_ACTION::UP);
	frame(map);
	frame(map);
	BOOST_TEST(seen.size() == 1u);
	BOOST_TEST(held(a, "Jump"));

	mode(ecs::run_mode::play);
	frame(map);
	frame(map);
	BOOST_TEST(count(inp::action_edge::released) == 1u);
	BOOST_TEST(count(inp::action_edge::pressed) == 1u);
	BOOST_TEST(!held(a, "Jump"));
}

BOOST_AUTO_TEST_CASE(AKeyPressedDuringPause_IsAPressOnPlayOnlyWhenTheActionWasArmed)
{
	world("a", 0);
	const auto map = jump_on_space();
	frame(map); // seen up while bound: armed

	mode(ecs::run_mode::pause);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(seen.empty());

	mode(ecs::run_mode::play);
	frame(map);
	frame(map);
	BOOST_TEST(count(inp::action_edge::pressed) == 1u);
	BOOST_TEST(count(inp::action_edge::released) == 0u);
}

BOOST_AUTO_TEST_CASE(AKeyHeldSinceBeforeTheWorldSawItUp_IsNoPressAfterPause)
{
	world("a", 0);
	const auto map = jump_on_space();
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map); // first sight: down, so not armed

	mode(ecs::run_mode::pause);
	frame(map);
	mode(ecs::run_mode::play);
	frame(map);
	frame(map);
	BOOST_TEST(seen.empty());
}

BOOST_AUTO_TEST_CASE(AStepWithoutTheBinding_KeepsThePausedState)
{
	auto& a = world("a", 0);
	const auto map = jump_on_space();
	frame(map);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_REQUIRE(held(a, "Jump"));

	mode(ecs::run_mode::pause);
	unbind();
	frame(map);
	mgr.get_set("level").step(1);
	frame(map);
	BOOST_TEST(bound_of(a).frozen);
	BOOST_TEST(held(a, "Jump"));
	BOOST_TEST(count(inp::action_edge::released) == 0u);
}

BOOST_AUTO_TEST_CASE(LosingTheBindingInPlay_LetsTheActionGoOnce)
{
	auto& a = world("a", 0);
	const auto map = jump_on_space();
	frame(map);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);

	unbind();
	frame(map);
	frame(map);
	BOOST_TEST(map_of(a).map == map);
	BOOST_TEST(count(inp::action_edge::released) == 1u);
	BOOST_TEST(!held(a, "Jump"));

	bind(); // back with the key still held: no press until it is let go
	frame(map);
	BOOST_TEST(count(inp::action_edge::pressed) == 1u);
	key(KB::SPACE, KEY_ACTION::UP);
	frame(map);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(count(inp::action_edge::pressed) == 2u);
}

BOOST_AUTO_TEST_CASE(InEdit_NothingIsCounted)
{
	auto& a = world("a", 0);
	mode(ecs::run_mode::edit);
	const auto map = jump_on_space();
	frame(map);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(seen.empty());
	BOOST_TEST(!held(a, "Jump"));
}

BOOST_AUTO_TEST_CASE(ASetWhereNoWorldRunsTheSystem_HasNoActions)
{
	auto& a = world("a", 0, { "inp::update_input_state" });
	const auto map = jump_on_space();
	frame(map);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(bound_of(a).bound);
	BOOST_TEST(!map_of(a).map);
	BOOST_TEST(!held(a, "Jump"));
	BOOST_TEST(seen.empty());
}

BOOST_AUTO_TEST_CASE(TheDescOnDisk_IsFollowedWithoutFalseEdges)
{
	auto& a = world("a", 0);
	gs::input_actions actions(*rs);
	const auto tag = put("actions.desc", ACTIONS_SPACE);
	load(actions, tag);
	BOOST_REQUIRE(actions.map());
	BOOST_TEST(actions.map()->size() == 1u);

	frame(actions.map());
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(actions.map());
	BOOST_REQUIRE_EQUAL(count(inp::action_edge::pressed), 1u);

	// Jump moves to J while SPACE is still held: what the old map held is let go once, the new one gives no press.
	put("actions.desc", ACTIONS_J);
	rs->signal_changed(tag);
	pump();
	load(actions, tag);
	frame(actions.map());
	frame(actions.map());
	BOOST_TEST(count(inp::action_edge::released) == 1u);
	BOOST_TEST(count(inp::action_edge::pressed) == 1u);
	BOOST_TEST(!held(a, "Jump"));

	key(KB::J, KEY_ACTION::DOWN);
	frame(actions.map());
	BOOST_TEST(count(inp::action_edge::pressed) == 2u);
}

BOOST_AUTO_TEST_CASE(ABrokenEdit_KeepsThePreviousActions)
{
	gs::input_actions actions(*rs);
	const auto tag = put("actions.desc", ACTIONS_SPACE);
	load(actions, tag);
	const auto before = actions.map();
	BOOST_REQUIRE(before);

	put("actions.desc", "{ not json");
	rs->signal_changed(tag);
	pump();
	load(actions, tag);
	BOOST_TEST(actions.map() == before);
}

BOOST_AUTO_TEST_CASE(NoTag_NoMap)
{
	gs::input_actions actions(*rs);
	actions.update(std::string_view{});
	BOOST_TEST(!actions.map());

	const auto tag = put("actions.desc", ACTIONS_SPACE);
	load(actions, tag);
	BOOST_REQUIRE(actions.map());
	actions.update(std::string_view{});
	BOOST_TEST(!actions.map());
}

BOOST_AUTO_TEST_CASE(Fixed_AFrameWithoutAStepThenOneWithTwo_ShowsThePressInTheFirstStepOnly)
{
	auto& w = fixed_world();
	const auto map = jump_on_space();
	frame(map, 0.025f); // two steps, 5 ms left over
	const int before = probe_of(w).steps;

	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map, 0.004f); // 9 ms: no fixed step
	BOOST_TEST(probe_of(w).steps == before);
	BOOST_TEST(count(inp::action_edge::pressed) == 1u); // the bus has it this frame

	frame(map, 0.016f); // 25 ms: two steps
	BOOST_TEST(probe_of(w).steps == before + 2);
	BOOST_TEST(probe_of(w).pressed == 1);
	BOOST_TEST(probe_of(w).first_pressed_step == before);
}

BOOST_AUTO_TEST_CASE(Fixed_APressDuringPause_IsSeenByTheStepOfAStep)
{
	auto& w = fixed_world();
	const auto map = jump_on_space();
	frame(map, 0.02f);

	mode(ecs::run_mode::pause);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(probe_of(w).pressed == 0);

	mgr.get_set("level").step(1);
	frame(map);
	BOOST_TEST(probe_of(w).pressed == 1);
	key(KB::SPACE, KEY_ACTION::UP);
	mgr.get_set("level").step(1);
	frame(map);
	BOOST_TEST(probe_of(w).released == 1);
}

BOOST_AUTO_TEST_SUITE_END()
