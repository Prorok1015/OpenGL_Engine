#include <boost/test/unit_test.hpp>
#include <snk/ecs/ecs_component_registry.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/ecs/ecs_system_plan.hpp>
#include <entt/entt.hpp>
#include <optional>
#include <string>
#include <vector>

// Layered ECS registries (#220 PR-3, MODULE_LAYER.md): a layer reads through to its parent, writes
// only to itself, cannot take the parent's names, and leaves the parent as it was when it goes away.

namespace {

std::vector<std::string> g_log;

template <char C>
void sys() { g_log.emplace_back(1, C); }

struct base_component {};
struct layer_component {};

using names = std::vector<std::string>;

ecs::system_plan build(const ecs::system_factory& sf, const names& list)
{
	return ecs::build_system_plan(sf.registry(), list, { .validate_access = false });
}

void run_all(const ecs::system_plan& plan)
{
	entt::registry reg;
	ecs::runtime_context ctx{ .current_registry = reg };
	for (const auto& phase : plan.phases) {
		for (const auto& node : phase.nodes) node(ctx);
	}
}

struct layer_fixture {
	layer_fixture()
	{
		g_log.clear();
		base.register_system<&sys<'b'>>("engine::b");
		base.add_game_phase("engine::ai", ecs::phase::update, ecs::phase_side::before);
		base.register_system<&sys<'a'>>("engine::a", { .phase = "engine::ai" });
		layer.emplace(&base);
	}

	ecs::system_factory base;
	std::optional<ecs::system_factory> layer;
};

}

BOOST_FIXTURE_TEST_SUITE(ecs_layered_registry_tests, layer_fixture)

BOOST_AUTO_TEST_CASE(layer_plan_runs_base_and_layer_systems)
{
	layer->register_system<&sys<'g'>>("game::g", { .after = { "engine::b" } });
	const auto plan = build(*layer, { "game::g", "engine::b", "engine::a" });
	BOOST_TEST(!plan.has_errors());
	run_all(plan);
	BOOST_TEST(g_log == (names{ "a", "b", "g" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(base_does_not_see_layer_systems)
{
	layer->register_system<&sys<'g'>>("game::g");
	BOOST_TEST(!base.registry().find("game::g"));
	BOOST_TEST(build(base, { "game::g" }).has_diagnostic(ecs::diagnostic_kind::unknown_system));
}

BOOST_AUTO_TEST_CASE(layer_cannot_take_a_base_name)
{
	layer->register_system<&sys<'x'>>("engine::b");
	BOOST_TEST(layer->registry().size() == 0u);
	run_all(build(*layer, { "engine::b" }));
	BOOST_TEST(g_log == (names{ "b" }), boost::test_tools::per_element());
	BOOST_TEST(!layer->unregister_system("engine::b"));
	BOOST_TEST(static_cast<bool>(base.registry().find("engine::b")));
}

BOOST_AUTO_TEST_CASE(game_phases_are_layered)
{
	BOOST_TEST(layer->registry().phases().contains(ecs::phase_id{ "engine::ai" }));
	BOOST_TEST(!layer->add_game_phase("engine::ai", ecs::phase::input, ecs::phase_side::after));
	BOOST_TEST(!layer->remove_game_phase("engine::ai"));
	BOOST_REQUIRE(layer->add_game_phase("game::late", ecs::phase::late_update, ecs::phase_side::after));
	layer->register_system<&sys<'l'>>("game::l", { .phase = "game::late" });
	layer->register_system<&sys<'i'>>("game::i", { .phase = "engine::ai" });

	BOOST_TEST(!base.registry().phases().contains(ecs::phase_id{ "game::late" }));
	BOOST_TEST(layer->registry().phases().game_phases().size() == 1u);
	BOOST_TEST(layer->registry().phases().all_game_phases().size() == 2u);
	BOOST_TEST(base.registry().phases().ordered().size() == ecs::builtin_phase_count + 1);
	BOOST_TEST(layer->registry().phases().ordered().size() == ecs::builtin_phase_count + 2);

	const auto plan = build(*layer, { "game::l", "game::i", "engine::b" });
	BOOST_TEST(!plan.has_errors());
	run_all(plan);
	BOOST_TEST(g_log == (names{ "i", "b", "l" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(base_is_unchanged_after_the_layer_goes_away)
{
	layer->register_system<&sys<'g'>>("game::g");
	layer->add_game_phase("game::late", ecs::phase::late_update, ecs::phase_side::after);
	layer.reset();

	BOOST_TEST(base.registry().size() == 2u);
	BOOST_TEST(base.registry().phases().all_game_phases().size() == 1u);
	run_all(build(base, { "engine::b", "engine::a" }));
	BOOST_TEST(g_log == (names{ "a", "b" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(component_names_are_layered)
{
	ecs::component_registry base_names;
	BOOST_REQUIRE(base_names.add<base_component>("engine.base"));
	{
		ecs::component_registry layer_names{ &base_names };
		BOOST_TEST((layer_names.find("engine.base") == entt::type_hash<base_component>::value()));
		BOOST_TEST(layer_names.add<layer_component>("game.layer"));
		BOOST_TEST(!layer_names.add<layer_component>("engine.base")); // name of the base
		BOOST_TEST(!layer_names.add<base_component>("game.alias"));   // id of the base
		BOOST_TEST(!layer_names.add("", 1u));
		BOOST_TEST(!layer_names.remove("engine.base"));
		BOOST_TEST(layer_names.size() == 1u);
		BOOST_REQUIRE(layer_names.name_of(entt::type_hash<layer_component>::value()) != nullptr);
		BOOST_TEST(*layer_names.name_of(entt::type_hash<layer_component>::value()) == "game.layer");
		BOOST_TEST(!base_names.find("game.layer"));
	}
	BOOST_TEST(base_names.size() == 1u);
	BOOST_TEST(!base_names.add<base_component>("engine.base"));
	BOOST_TEST(base_names.remove("engine.base"));
	BOOST_TEST(base_names.add<base_component>("engine.base"));
}

BOOST_AUTO_TEST_SUITE_END()
