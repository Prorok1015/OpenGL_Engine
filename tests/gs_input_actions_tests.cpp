#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/game/gs_game_module.h>
#include <snk/game/gs_input_actions.h>
#include <snk/game/gs_input_routing.h>
#include <snk/application/app_loaded_module_apis.h>
#include <snk/application/app_module_entry.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/input/inp_action_state.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/input/inp_events.hpp>
#include <snk/input/inp_input_router.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/ecs/ecs_fixed_stepper.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
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

const std::vector<std::string> WITH_STACK_SCRIPT{ "inp::update_input_state", "inp::update_actions", "test::stack_script" };

// What a game system would do with the stack, set by the test and run once by the system in the next update.
struct stack_script
{
	std::function<void(inp::input_stack_control&)> run;
};

void stack_script_system(inp::input_stack_control& routes, stack_script& script)
{
	if (!script.run) return;
	script.run(routes);
	script.run = nullptr;
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
		sf.register_system<&stack_script_system>("test::stack_script", { .phase = ecs::phase::update });
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

	// A world with a system that runs what the test sets with run_in, like game code asking for changes of the stack.
	scn::world_context& script_world(std::string_view name, uint32_t desc_index)
	{
		auto& w = world(name, desc_index, WITH_STACK_SCRIPT);
		w.state().ctx().emplace<stack_script>();
		return w;
	}
	static void run_in(scn::world_context& w, std::function<void(inp::input_stack_control&)> fn)
	{
		w.state().ctx().get<stack_script>().run = std::move(fn);
	}
	static bool key_in(scn::world_context& w, KB k)
	{
		return w.state().ctx().get<inp::input_state>().keyboard[static_cast<std::size_t>(k)];
	}
	bool world_on(std::string_view name) { return router.stack("level").world_active(name); }

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

BOOST_AUTO_TEST_CASE(OneBindingHeldAndTakenForGood_TheOtherOneStillGivesAPress)
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

	// A is taken by the world above and held for good; the left arrow is pressed later.
	key(KB::A, KEY_ACTION::DOWN);
	frame(map);
	frame(map);
	BOOST_TEST(!held(game, "MoveLeft"));
	key(KB::LEFT, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(pressed(game, "MoveLeft"));
	BOOST_TEST(count(inp::action_edge::pressed) == 1u);
	key(KB::LEFT, KEY_ACTION::UP);
	frame(map);
	BOOST_TEST(count(inp::action_edge::released) == 1u);
}

BOOST_AUTO_TEST_CASE(AKeyHeldThroughAModalWorld_IsNoPressWhenTheWorldIsOffAndAPressAfterItIsLetGo)
{
	auto& game = world("game", 0);
	auto& menu = world("menu", 1, { "inp::update_input_state" });
	scn::input_stack_desc modal;
	modal.order = 5;
	modal.block_below = true;
	menu.set_input_stack(modal);
	const auto map = jump_on_space();
	frame(map);

	// Pressed while the menu blocks, still held when the menu goes.
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	modal.active = false;
	menu.set_input_stack(modal);
	frame(map);
	frame(map);
	BOOST_TEST(!held(game, "Jump"));
	BOOST_TEST(seen.empty());

	key(KB::SPACE, KEY_ACTION::UP);
	frame(map);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(pressed(game, "Jump"));
	BOOST_REQUIRE_EQUAL(seen.size(), 1u);
}

BOOST_AUTO_TEST_CASE(ANewWorldOnAHeldKey_DoesNotSeeAPress_AndTheDeathOfAWorldDropsItsMemory)
{
	const auto map = jump_on_space();
	// The key is already held when the world that reads the actions comes up.
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	auto& game = world("game", 0);
	frame(map);
	frame(map);
	BOOST_TEST(!held(game, "Jump"));
	BOOST_TEST(seen.empty());
	const std::size_t memories = router.stack("level").memory_count();
	BOOST_TEST(memories >= 1u);

	// Let go and pressed again: a press.
	key(KB::SPACE, KEY_ACTION::UP);
	frame(map);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(pressed(game, "Jump"));

	// Another world comes up on the held key and goes again with its memory.
	world("menu", 1, { "inp::update_input_state" });
	frame(map);
	BOOST_TEST(router.stack("level").memory_count() > memories);
	mgr.get_set("level").destroy_context("menu");
	frame(map);
	frame(map);
	BOOST_TEST(router.stack("level").memory_count() == memories);
}

BOOST_AUTO_TEST_CASE(AKeyPressedInTheFrameTheModalWorldGoesOff_IsAPress)
{
	auto& game = world("game", 0);
	auto& menu = world("menu", 1, { "inp::update_input_state" });
	scn::input_stack_desc modal;
	modal.order = 5;
	modal.block_below = true;
	menu.set_input_stack(modal);
	const auto map = jump_on_space();
	frame(map);
	frame(map);

	modal.active = false;
	menu.set_input_stack(modal);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(pressed(game, "Jump"));
	BOOST_REQUIRE_EQUAL(seen.size(), 1u);
}

BOOST_AUTO_TEST_CASE(AKeyHeldAcrossAFocusChange_DoesNotStopThePressOfAnotherBinding)
{
	auto& game = world("game", 0);
	auto map = std::make_shared<inp::action_map>();
	map->add("MoveLeft", { KB::A, KB::LEFT });
	frame(map);

	// A is pressed while the view is not bound and is still held when it is: no press through A.
	unbind();
	key(KB::A, KEY_ACTION::DOWN);
	frame(map);
	bind();
	frame(map);
	BOOST_TEST(!held(game, "MoveLeft"));
	BOOST_TEST(seen.empty());

	// The left arrow is another binding: it presses.
	key(KB::LEFT, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(pressed(game, "MoveLeft"));
	BOOST_TEST(count(inp::action_edge::pressed) == 1u);
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

BOOST_AUTO_TEST_CASE(TheUserBindings_AreAppliedKeepTheIdsAndSurviveAnEditOfTheDesc)
{
	gs::input_actions actions(*rs);
	const auto folder = std::filesystem::temp_directory_path() / ("snk_gs_user_bindings_" + std::to_string(reinterpret_cast<std::uintptr_t>(&actions)));
	std::filesystem::remove_all(folder);
	actions.set_user_folder(folder);
	const auto tag = put("actions.desc", ACTIONS_SPACE);
	actions.update(tag.view(), "Game");
	pump();
	actions.update(tag.view(), "Game");
	BOOST_REQUIRE(actions.map());
	const inp::action_id jump = actions.map()->id("Jump");

	BOOST_REQUIRE(actions.rebind("Jump", 0, KB::K).ok());
	BOOST_TEST((actions.map()->find(jump)->bindings == std::vector<inp::action_binding>{ KB::K }));
	BOOST_TEST((actions.project_map()->find(jump)->bindings == std::vector<inp::action_binding>{ KB::SPACE }));
	std::size_t written = 0;
	for (const auto& entry : std::filesystem::directory_iterator(folder)) {
		BOOST_TEST(entry.path().extension() == ".json");
		++written;
	}
	BOOST_TEST(written == 1u);

	put("actions.desc", ACTIONS_J);
	rs->signal_changed(tag);
	pump();
	actions.update(tag.view(), "Game");
	pump();
	actions.update(tag.view(), "Game");
	BOOST_TEST((actions.map()->find(jump)->bindings == std::vector<inp::action_binding>{ KB::K }));
	BOOST_TEST((actions.project_map()->find(jump)->bindings == std::vector<inp::action_binding>{ KB::J }));

	gs::input_actions again(*rs);
	again.set_user_folder(folder);
	again.update(tag.view(), "Game");
	pump();
	again.update(tag.view(), "Game");
	BOOST_REQUIRE(again.map());
	BOOST_TEST((again.map()->find(jump)->bindings == std::vector<inp::action_binding>{ KB::K }));

	BOOST_TEST(again.reset_binding("Jump"));
	BOOST_TEST((again.map()->find(jump)->bindings == std::vector<inp::action_binding>{ KB::J }));
	std::filesystem::remove_all(folder);
}

BOOST_AUTO_TEST_CASE(AChangeTheFileCannotTake_IsAppliedAndSaysItWasNotSaved)
{
	gs::input_actions actions(*rs);
	const auto base = std::filesystem::temp_directory_path() / ("snk_gs_user_blocker_" + std::to_string(reinterpret_cast<std::uintptr_t>(&actions)));
	std::filesystem::remove_all(base);
	std::filesystem::create_directories(base);
	std::ofstream(base / "blocker") << "a file where a folder should be";
	actions.set_user_folder(base / "blocker" / "bindings");
	const auto tag = put("actions.desc", ACTIONS_SPACE);
	actions.update(tag.view(), "Game");
	pump();
	actions.update(tag.view(), "Game");
	BOOST_REQUIRE(actions.map());

	const gs::rebind_outcome outcome = actions.rebind("Jump", 0, KB::K);
	BOOST_TEST(outcome.ok());
	BOOST_TEST(!outcome.saved);
	BOOST_TEST(!actions.user_file_saved());
	BOOST_TEST((actions.map()->find(actions.map()->id("Jump"))->bindings == std::vector<inp::action_binding>{ KB::K }));
	std::filesystem::remove_all(base);
}

BOOST_AUTO_TEST_CASE(AMapWithGamepadBindings_WarnsOncePerMapAboutModulesOfAnOlderApi)
{
	std::vector<std::string> warnings;
	const auto sink = engine::add_log_sink([&warnings](std::string_view, engine::log_level level, std::string_view message) {
		if (level == engine::log_level::warn) warnings.emplace_back(message);
	});
	ds::app_data_storage storage;
	storage.construct<app::modules::loaded_module_apis>().api = { { "old_game", 0x0003'000Fu }, { "new_game", SNK_MODULE_API_VERSION } };

	gs::input_actions actions(*rs);
	const auto tag = put("actions.desc", R"json({"__type":"input_actions_desc","__version":1,"actions":[
		{"name":"Jump","bindings":[{"key":"SPACE"},{"gamepad":"A"}]}]})json");
	load(actions, tag);
	BOOST_REQUIRE(actions.map());
	BOOST_TEST(actions.has_gamepad_bindings());

	actions.warn_about_old_modules(storage);
	actions.warn_about_old_modules(storage);
	std::size_t about_old = 0;
	for (const auto& w : warnings) {
		if (w.find("old_game (api 3.15)") != std::string::npos) ++about_old;
		BOOST_TEST(w.find("new_game") == std::string::npos);
	}
	BOOST_TEST(about_old == 1u);

	const auto without = put("keys.desc", ACTIONS_SPACE);
	gs::input_actions keys_only(*rs);
	load(keys_only, without);
	BOOST_TEST(!keys_only.has_gamepad_bindings());
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

// ---------------------------------------------------------------- the stack from code (#625)

BOOST_AUTO_TEST_CASE(StackCommands_ASystemSwitchesAWorldOff_FromTheNextFrame)
{
	auto& game = script_world("game", 0);
	auto& minimap = world("minimap", 1, { "inp::update_input_state" });
	const auto map = jump_on_space();
	frame(map);
	key(KB::W, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(world_on("minimap"));
	BOOST_TEST(key_in(minimap, KB::W));

	run_in(game, [](inp::input_stack_control& routes) { routes.set_world_active("minimap", false); });
	frame(map);
	BOOST_TEST(world_on("minimap"));
	BOOST_TEST(key_in(minimap, KB::W));

	frame(map);
	BOOST_TEST(!world_on("minimap"));
	BOOST_TEST(!key_in(minimap, KB::W));
	BOOST_TEST(key_in(game, KB::W));

	run_in(game, [](inp::input_stack_control& routes) { routes.set_world_active("minimap", true); });
	frame(map);
	frame(map);
	BOOST_TEST(world_on("minimap"));
	BOOST_TEST(key_in(minimap, KB::W));
}

BOOST_AUTO_TEST_CASE(StackCommands_ASystemPushesALayerOfItsWorld_AndARepeatedPushMakesNoSecondOne)
{
	auto& game = script_world("game", 0);
	const auto map = jump_on_space();
	frame(map);
	const std::size_t before = router.stack("level").size();

	const auto push_menu = [](inp::input_stack_control& routes) {
		inp::stack_layer_desc menu;
		menu.name = "menu";
		menu.order = 10;
		menu.block_below = true;
		routes.push(menu);
	};
	run_in(game, push_menu);
	frame(map);
	BOOST_TEST(router.stack("level").find("game", "menu") == nullptr);
	frame(map);
	const auto* menu = router.stack("level").find("game", "menu");
	BOOST_REQUIRE(menu != nullptr);
	BOOST_TEST((menu->owner == inp::layer_owner::module));
	BOOST_TEST(router.stack("level").size() == before + 1u);
	BOOST_TEST(menu->block_below);
	BOOST_TEST(menu->order == 10);

	run_in(game, push_menu);
	frame(map);
	frame(map);
	BOOST_TEST(router.stack("level").size() == before + 1u);
}

BOOST_AUTO_TEST_CASE(StackCommands_TheSystemsOfTheWorldsGoInTheOrderOfTheFile_NotOfTheIds_ThenTheExternalOnes)
{
	// "b" is made first, so its id is lower; the file puts "a" first.
	auto& b = script_world("b", 1);
	auto& a = script_world("a", 0);
	world("target", 2, { "inp::update_input_state" });
	const auto map = jump_on_space();
	frame(map);

	run_in(a, [](inp::input_stack_control& routes) { routes.set_world_active("target", false); });
	run_in(b, [](inp::input_stack_control& routes) { routes.set_world_active("target", true); });
	frame(map);
	frame(map);
	BOOST_TEST(world_on("target"));

	run_in(a, [](inp::input_stack_control& routes) { routes.set_world_active("target", true); });
	run_in(b, [](inp::input_stack_control& routes) { routes.set_world_active("target", true); });
	frame(map);
	gs::input_stack_handle(&router, "level").set_world_active("target", false);
	frame(map);
	BOOST_TEST(!world_on("target"));
}

BOOST_AUTO_TEST_CASE(StackCommands_PauseFreezesTheStack_TheCommandsWaitForTheEnd)
{
	auto& game = script_world("game", 0);
	world("minimap", 1, { "inp::update_input_state" });
	const auto map = jump_on_space();
	frame(map);

	run_in(game, [](inp::input_stack_control& routes) { routes.set_world_active("minimap", false); });
	frame(map);
	gs::input_stack_handle(&router, "level").push("game", [] { inp::stack_layer_desc l; l.name = "menu"; return l; }());
	mode(ecs::run_mode::pause);
	frame(map);
	frame(map);
	BOOST_TEST(world_on("minimap"));
	BOOST_TEST(router.stack("level").find("game", "menu") == nullptr);

	mode(ecs::run_mode::play);
	frame(map);
	BOOST_TEST(!world_on("minimap"));
	BOOST_TEST(router.stack("level").find("game", "menu") != nullptr);
}

BOOST_AUTO_TEST_CASE(StackCommands_TheModuleContextGivesTheStackOfASetThatIsThere_AndNothingForOneThatIsNot)
{
	world("game", 0);
	world("minimap", 1, { "inp::update_input_state" });
	const auto map = jump_on_space();
	frame(map);

	auto& layer = mgr.create_layer();
	gs::module_context context(layer, nullptr, nullptr, &router, &mgr);
	BOOST_TEST(!context.input_stack("play"));
	BOOST_TEST(!gs::module_context(layer, nullptr, nullptr, nullptr, &mgr).input_stack("level"));

	auto stack = context.input_stack("level");
	BOOST_REQUIRE(stack);
	stack.set_world_active("minimap", false);
	BOOST_TEST(world_on("minimap"));
	frame(map);
	BOOST_TEST(!world_on("minimap"));

	auto menu = inp::stack_layer_desc{};
	menu.name = "menu";
	stack.push("game", menu);
	stack.push("game", menu);
	frame(map);
	const auto* pushed = router.stack("level").find("game", "menu");
	BOOST_REQUIRE(pushed != nullptr);
	BOOST_TEST((pushed->owner == inp::layer_owner::module));
	BOOST_TEST(router.stack("level").size() == 3u);
}

BOOST_AUTO_TEST_CASE(StackCommands_AfterTheModuleUnloads_NoLayerOfItIsInAnySet_AndItsQueueIsEmpty)
{
	world("game", 0);
	const auto map = jump_on_space();
	frame(map);

	auto& layer = mgr.create_layer();
	gs::module_context context(layer, nullptr, nullptr, &router, &mgr);
	auto stack = context.input_stack("level");
	inp::stack_layer_desc menu;
	menu.name = "menu";
	stack.push("game", menu);
	frame(map);
	BOOST_REQUIRE(router.stack("level").find("game", "menu") != nullptr);
	stack.set_world_active("game", false);

	mgr.destroy_layer(layer);

	BOOST_TEST(router.stack("level").find("game", "menu") == nullptr);
	BOOST_TEST(router.stack("level").find("game", "world") != nullptr);
	BOOST_TEST(router.take_commands("level").empty());
}

BOOST_AUTO_TEST_CASE(StackCommands_AModuleCannotRemoveALayerOfTheEngineOrTheEditor)
{
	world("game", 0);
	const auto map = jump_on_space();
	frame(map);
	inp::stack_layer_desc panel;
	panel.name = "panel";
	panel.owner = inp::layer_owner::editor;
	router.stack("level").put("game", 0, panel);

	auto& layer = mgr.create_layer();
	auto stack = gs::module_context(layer, nullptr, nullptr, &router, &mgr).input_stack("level");
	BOOST_REQUIRE(stack);
	stack.remove("game", "panel");
	stack.remove("game", "world");
	stack.set_active("game", "panel", false);
	stack.set_order("game", "panel", 99);
	inp::stack_layer_desc takeover;
	takeover.name = "panel";
	stack.push("game", takeover);
	frame(map);

	const auto* kept = router.stack("level").find("game", "panel");
	BOOST_REQUIRE(kept != nullptr);
	BOOST_TEST((kept->owner == inp::layer_owner::editor));
	BOOST_TEST(kept->active);
	BOOST_TEST(kept->order == 0);
	BOOST_TEST(router.stack("level").find("game", "world") != nullptr);
}

// ---------------------------------------------------------------- inp::input_router::inject (#624)

BOOST_AUTO_TEST_CASE(Inject_IsSeenByThePollTheBusAndTheFixedSteps_FromTheNextRoutedFrame)
{
	auto& w = fixed_world();
	const auto map = jump_on_space();
	frame(map, 0.02f);

	router.inject({ "level", "a" }, "Jump", inp::action_edge::pressed);
	BOOST_TEST(!held(w, "Jump"));
	BOOST_TEST(seen.empty());

	frame(map, 0.02f);
	BOOST_TEST(pressed(w, "Jump"));
	BOOST_TEST(held(w, "Jump"));
	BOOST_REQUIRE_EQUAL(seen.size(), 1u);
	BOOST_TEST(seen[0].world == "a");
	BOOST_TEST(seen[0].action == map->id("Jump"));
	BOOST_TEST((seen[0].edge == inp::action_edge::pressed));
	BOOST_TEST(probe_of(w).pressed == 1);

	frame(map, 0.02f);
	BOOST_TEST(held(w, "Jump"));
	BOOST_TEST(!pressed(w, "Jump"));
	BOOST_TEST(seen.size() == 1u);

	router.inject({ "level", "a" }, "Jump", inp::action_edge::released);
	frame(map, 0.02f);
	BOOST_TEST(!held(w, "Jump"));
	BOOST_TEST(count(inp::action_edge::released) == 1u);
	BOOST_TEST(probe_of(w).released == 1);
}

BOOST_AUTO_TEST_CASE(Inject_APauseKeepsTheEdgesInTheQueue_AndTheReleaseAfterItLetsGoOnce)
{
	auto& w = world("a", 0);
	const auto map = jump_on_space();
	frame(map);

	router.inject({ "level", "a" }, "Jump", inp::action_edge::pressed);
	frame(map);
	BOOST_TEST(held(w, "Jump"));

	mode(ecs::run_mode::pause);
	router.inject({ "level", "a" }, "Jump", inp::action_edge::released);
	frame(map);
	frame(map);
	BOOST_TEST(held(w, "Jump"));

	mode(ecs::run_mode::play);
	frame(map);
	BOOST_TEST(!held(w, "Jump"));
	frame(map);
	BOOST_TEST(!held(w, "Jump"));
	BOOST_TEST(count(inp::action_edge::pressed) == 1u);
	BOOST_TEST(count(inp::action_edge::released) == 1u);
}

BOOST_AUTO_TEST_CASE(Inject_AWorldFrozenInAStepKeepsItsEdges_AndGetsThemWhenItRunsAgain)
{
	auto& w = world("a", 0);
	const auto map = jump_on_space();
	unbind();
	frame(map);
	router.inject({ "level", "a" }, "Jump", inp::action_edge::pressed);
	frame(map);
	BOOST_TEST(held(w, "Jump"));

	mode(ecs::run_mode::pause);
	router.inject({ "level", "a" }, "Jump", inp::action_edge::released);
	frame(map);
	mgr.get_set("level").step(1);
	frame(map);
	BOOST_TEST(bound_of(w).frozen);
	BOOST_TEST(held(w, "Jump"));
	BOOST_TEST(count(inp::action_edge::released) == 0u);

	mode(ecs::run_mode::play);
	frame(map);
	BOOST_TEST(!held(w, "Jump"));
	BOOST_TEST(count(inp::action_edge::released) == 1u);
}

BOOST_AUTO_TEST_CASE(Inject_IntoASetThatIsNotThere_IsDroppedAndNotGivenToTheNextOneOfThatName)
{
	world("a", 0);
	const auto map = jump_on_space();
	frame(map);

	router.inject({ "later", "a" }, "Jump", inp::action_edge::pressed);
	frame(map);
	BOOST_TEST(router.take_injected("later").empty());

	mgr.create_set("later");
	auto& later = mgr.create_context("later", "a", scn::context_kind::level, WITH_ACTIONS);
	later.set_desc_index(0);
	router.set_default_target(inp::input_target{ .set = "later" });
	frame(map);
	frame(map);
	BOOST_TEST(!held(later, "Jump"));
	BOOST_TEST(seen.empty());
}

BOOST_AUTO_TEST_CASE(Inject_NeedsNeitherAKeyNorTheInputOfTheWorld)
{
	auto& w = world("a", 0);
	unbind();
	const auto map = jump_on_space();
	frame(map);

	router.inject({ "level", "a" }, "Jump", inp::action_edge::pressed);
	frame(map);
	BOOST_TEST(!bound_of(w).bound);
	BOOST_TEST(pressed(w, "Jump"));
	BOOST_TEST(count(inp::action_edge::pressed) == 1u);

	frame(map);
	BOOST_TEST(held(w, "Jump"));
}

BOOST_AUTO_TEST_CASE(Inject_ARealPressOfTheSameActionGivesNoSecondPress)
{
	auto& w = world("a", 0);
	const auto map = jump_on_space();
	frame(map);

	router.inject({ "level", "a" }, "Jump", inp::action_edge::pressed);
	frame(map);
	key(KB::SPACE, KEY_ACTION::DOWN);
	frame(map);
	BOOST_TEST(held(w, "Jump"));
	BOOST_TEST(count(inp::action_edge::pressed) == 1u);
}

BOOST_AUTO_TEST_CASE(Inject_IntoAWorldThatReadsNoActions_ANameTheMapDoesNotHaveOrAMissingWorld_IsDropped)
{
	auto& reader = world("reader", 0);
	auto& other = world("other", 1);
	const auto map = jump_on_space();
	frame(map);

	router.inject({ "level", "other" }, "Jump", inp::action_edge::pressed);
	router.inject({ "level", "reader" }, "Nope", inp::action_edge::pressed);
	router.inject({ "level", "ghost" }, "Jump", inp::action_edge::pressed);
	router.inject({ "level", "" }, "Jump", inp::action_edge::pressed);
	frame(map);
	frame(map);

	BOOST_TEST(!held(reader, "Jump"));
	BOOST_TEST(!held(other, "Jump"));
	BOOST_TEST(seen.empty());
}

BOOST_AUTO_TEST_CASE(Inject_ADroppedEdgeIsNotKeptForALaterFrame)
{
	auto& w = world("a", 0);
	const auto map = jump_on_space();
	frame(map);

	mode(ecs::run_mode::edit);
	router.inject({ "level", "a" }, "Jump", inp::action_edge::pressed);
	frame(map);
	mode(ecs::run_mode::play);
	frame(map);
	BOOST_TEST(!held(w, "Jump"));
	BOOST_TEST(seen.empty());
}

BOOST_AUTO_TEST_CASE(StackCommands_ForASetThatIsNotThere_AreDroppedAndNotGivenToTheNextOneOfThatName)
{
	world("a", 0);
	const auto map = jump_on_space();
	frame(map);

	router.queue_command("later", { .kind = inp::stack_command_kind::set_world_active, .world = "a", .active = false });
	frame(map);
	BOOST_TEST(router.take_commands("later").empty());

	mgr.create_set("later");
	mgr.create_context("later", "a", scn::context_kind::level, WITH_ACTIONS).set_desc_index(0);
	router.set_default_target(inp::input_target{ .set = "later" });
	frame(map);
	frame(map);
	BOOST_TEST(router.stack("later").world_active("a"));
}

BOOST_AUTO_TEST_CASE(StackCommands_AWorldMadeAgainInTheFrame_KeepsTheSwitchAskedForThatFrame)
{
	world("game", 0);
	world("minimap", 1, { "inp::update_input_state" });
	const auto map = jump_on_space();
	frame(map);
	BOOST_TEST(world_on("minimap"));

	mgr.destroy_context("level", "minimap");
	world("minimap", 5, { "inp::update_input_state" });
	gs::input_stack_handle(&router, "level").set_world_active("minimap", false);
	frame(map);
	BOOST_TEST(!world_on("minimap"));
}

BOOST_AUTO_TEST_CASE(StackCommands_ALongPauseKeepsAtMostTheLimit_AndAppliesThemWhenItGoes)
{
	world("game", 0);
	const auto map = jump_on_space();
	frame(map);
	const std::size_t before = router.stack("level").size();

	mode(ecs::run_mode::pause);
	gs::input_stack_handle handle(&router, "level");
	for (std::size_t i = 0; i < inp::input_router::command_limit + 40; ++i) {
		inp::stack_layer_desc layer;
		layer.name = "l" + std::to_string(i);
		handle.push("game", layer);
	}
	frame(map);
	frame(map);
	BOOST_TEST(router.stack("level").size() == before);

	mode(ecs::run_mode::play);
	frame(map);
	BOOST_TEST(router.stack("level").size() == before + inp::input_router::command_limit);
}

BOOST_AUTO_TEST_SUITE_END()
