#include <boost/test/unit_test.hpp>
#include <snk/ecs/ecs_system.h>
#include <entt/entt.hpp>

// Phase-based registration: {phase, after, before, run_in_edit} declared by the system author.

namespace {

struct counter { int value = 0; };

void sys_bump(counter& c) { ++c.value; }

struct stateful_bump : ecs::system_interface {
	explicit stateful_bump(int step) : step(step) {}
	void run(counter& c) { c.value += step; }
	int step;
};

ecs::runtime_context make_ctx(entt::registry& reg)
{
	return ecs::runtime_context{ .current_registry = reg };
}

}

BOOST_AUTO_TEST_SUITE(EcsSystemRegistrationTests)

BOOST_AUTO_TEST_CASE(Register_StoresPhaseConstraintsAndEditFlag)
{
	ecs::system_factory sf;
	sf.register_system<&sys_bump>("late", {
		.phase = ecs::phase::transform,
		.after = { "a", "b" },
		.before = { "c" },
		.run_in_edit = true });

	const auto* entry = sf.registry().find("late");
	BOOST_REQUIRE(entry != nullptr);
	BOOST_TEST((entry->desc.phase == ecs::phase::transform));
	BOOST_TEST((entry->desc.after == std::vector<std::string>{ "a", "b" }), boost::test_tools::per_element());
	BOOST_TEST((entry->desc.before == std::vector<std::string>{ "c" }), boost::test_tools::per_element());
	BOOST_TEST(entry->desc.run_in_edit);
}

BOOST_AUTO_TEST_CASE(Register_DefaultsToUpdatePhaseAndNoEdit)
{
	ecs::system_factory sf;
	sf.register_system<&sys_bump>("plain");

	const auto* entry = sf.registry().find("plain");
	BOOST_REQUIRE(entry != nullptr);
	BOOST_TEST((entry->desc.phase == ecs::phase::update));
	BOOST_TEST(!entry->desc.run_in_edit);
	BOOST_TEST(sf.registry().find("missing") == nullptr);
}

BOOST_AUTO_TEST_CASE(Register_GamePhaseCanBeUsedByName)
{
	ecs::system_factory sf;
	BOOST_TEST(sf.add_game_phase("ai", ecs::phase::update, ecs::phase_side::before));
	sf.register_system<&sys_bump>("think", { .phase = "ai" });

	const auto* entry = sf.registry().find("think");
	BOOST_REQUIRE(entry != nullptr);
	BOOST_TEST(!entry->desc.phase.is_builtin());
	BOOST_TEST(sf.registry().phases().contains(entry->desc.phase));
}

BOOST_AUTO_TEST_CASE(Entry_RunsStatelessAndBothStatefulFlavors)
{
	ecs::system_factory sf;
	sf.register_system<&sys_bump>("fn");
	sf.register_system<&stateful_bump::run, stateful_bump>("in_place", {}, 10);
	auto shared = std::make_shared<stateful_bump>(100);
	sf.register_system<&stateful_bump::run>("shared", {}, shared);

	entt::registry reg;
	auto ctx = make_ctx(reg);
	auto& c = reg.ctx().emplace<counter>();

	auto run = [&](const char* name) {
		const auto* entry = sf.registry().find(name);
		BOOST_REQUIRE(entry != nullptr);
		auto inst = entry->make();
		entry->run(inst.self, ctx);
		return inst;
	};

	run("fn");
	BOOST_TEST(c.value == 1);
	auto a = run("in_place");
	auto b = run("in_place");
	BOOST_TEST(c.value == 21);
	BOOST_TEST((a.owner != b.owner)); // one instance per world
	auto s1 = run("shared");
	auto s2 = run("shared");
	BOOST_TEST(c.value == 221);
	BOOST_TEST((s1.owner == s2.owner)); // shared across worlds
}

BOOST_AUTO_TEST_SUITE_END()
