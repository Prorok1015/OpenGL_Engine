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
		state.update(ref, keys, bound, nullptr);
	}

	void set(KB key, bool down) { keys.keyboard[static_cast<std::size_t>(key)] = down; }
	void set(MB button, bool down) { keys.mouse[static_cast<std::size_t>(button)] = down; }

	bool edge_free(inp::action_id id) const { return !state.pressed(id) && !state.released(id); }

	inp::action_map_ref ref{ make_map() };
	inp::input_state keys{};
	inp::input_bound bound{ .bound = true, .frozen = false };
	inp::action_state state{};
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

BOOST_AUTO_TEST_SUITE_END()
