#include <boost/test/unit_test.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/ecs/ecs_system_plan.hpp>
#include <snk/ecs/ecs_system_scope.hpp>
#include <snk/common/engine_assert.h>
#include <entt/entt.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Removable system and game-phase registrations (#220 PR-2): what a game module registers goes away
// with its scope, and the same names can be registered again.

namespace {

int g_runs = 0;

void sys_count() { ++g_runs; }

struct stateful_count : ecs::system_interface {
	explicit stateful_count(int step) : step(step) {}
	void run() { g_runs += step; }
	int step;
};

using names = std::vector<std::string>;

struct scope_fixture {
	scope_fixture() { g_runs = 0; }

	ecs::system_plan build(const names& list) const
	{
		return ecs::build_system_plan(sf.registry(), list, { .validate_access = false });
	}

	static void run_all(const ecs::system_plan& plan)
	{
		entt::registry reg;
		ecs::runtime_context ctx{ .current_registry = reg };
		for (const auto& phase : plan.phases) {
			for (const auto& node : phase.nodes) node(ctx);
		}
	}

	ecs::system_factory sf;
};

}

BOOST_FIXTURE_TEST_SUITE(ecs_system_scope_tests, scope_fixture)

BOOST_AUTO_TEST_CASE(scope_end_removes_its_systems_and_game_phases)
{
	sf.register_system<&sys_count>("engine::kept");
	{
		ecs::system_scope scope{ sf };
		BOOST_REQUIRE(scope.game_phase("game::ai", ecs::phase::update, ecs::phase_side::before));
		scope.system<&sys_count>("game::free", { .phase = "game::ai" });
		scope.system<&stateful_count::run, stateful_count>("game::shared", {}, std::make_shared<stateful_count>(10));
		scope.system<&stateful_count::run, stateful_count>("game::per_world", {}, 100);
		BOOST_TEST(scope.size() == 4u);
		BOOST_TEST(sf.registry().size() == 4u);
		BOOST_TEST(sf.registry().phases().contains(ecs::phase_id{ "game::ai" }));
	}
	BOOST_TEST(sf.registry().size() == 1u);
	BOOST_TEST(static_cast<bool>(sf.registry().find("engine::kept")));
	BOOST_TEST(!sf.registry().find("game::free"));
	BOOST_TEST(!sf.registry().find("game::shared"));
	BOOST_TEST(!sf.registry().find("game::per_world"));
	BOOST_TEST(!sf.registry().phases().contains(ecs::phase_id{ "game::ai" }));
	BOOST_TEST(sf.registry().phases().game_phases().empty());
}

BOOST_AUTO_TEST_CASE(names_can_be_registered_again_after_the_scope_ends)
{
	{
		ecs::system_scope first{ sf };
		first.game_phase("game::ai", ecs::phase::update, ecs::phase_side::after);
		first.system<&sys_count>("game::free", { .phase = "game::ai" });
	}
	ecs::system_scope second{ sf };
	BOOST_TEST(second.game_phase("game::ai", ecs::phase::update, ecs::phase_side::after));
	second.system<&sys_count>("game::free", { .phase = "game::ai" });

	run_all(build({ "game::free" }));
	BOOST_TEST(g_runs == 1);
}

BOOST_AUTO_TEST_CASE(plan_rebuilt_after_the_scope_ends_drops_the_systems)
{
	sf.register_system<&sys_count>("engine::kept");
	std::optional<ecs::system_scope> scope{ std::in_place, sf };
	scope->system<&stateful_count::run, stateful_count>("game::per_world", {}, 100);

	const names world{ "engine::kept", "game::per_world" };
	run_all(build(world));
	BOOST_TEST(g_runs == 101);

	scope.reset();
	const auto rebuilt = build(world);
	BOOST_TEST(rebuilt.system_count() == 1u);
	BOOST_TEST(rebuilt.has_diagnostic(ecs::diagnostic_kind::unknown_system));
	g_runs = 0;
	run_all(rebuilt);
	BOOST_TEST(g_runs == 1);
}

BOOST_AUTO_TEST_CASE(rejected_game_phase_is_not_recorded)
{
	ecs::system_scope scope{ sf };
	BOOST_TEST(!scope.game_phase("update", ecs::phase::update, ecs::phase_side::after)); // built-in name
	BOOST_TEST(scope.game_phase("game::ai", ecs::phase::update, ecs::phase_side::after));
	BOOST_TEST(!scope.game_phase("game::ai", ecs::phase::input, ecs::phase_side::before)); // taken
	BOOST_TEST(scope.size() == 1u);
}

BOOST_AUTO_TEST_CASE(moved_scope_keeps_the_registrations_until_its_last_owner_ends)
{
	std::optional<ecs::system_scope> outer;
	{
		ecs::system_scope inner{ sf };
		inner.system<&sys_count>("game::free");
		outer.emplace(std::move(inner));
		BOOST_TEST(inner.size() == 0u);
	}
	BOOST_TEST(static_cast<bool>(sf.registry().find("game::free")));
	outer.reset();
	BOOST_TEST(!sf.registry().find("game::free"));
}

BOOST_AUTO_TEST_CASE(remove_reports_unknown_names)
{
	BOOST_TEST(!sf.unregister_system("nope"));
	BOOST_TEST(!sf.remove_game_phase("nope"));
	BOOST_TEST(!sf.remove_game_phase("update")); // built-ins stay
}

BOOST_AUTO_TEST_CASE(removed_game_phase_disappears_from_the_rebuilt_plan)
{
	std::optional<ecs::system_scope> scope{ std::in_place, sf };
	scope->game_phase("game::ai", ecs::phase::update, ecs::phase_side::before);
	scope->system<&sys_count>("game::free", { .phase = "game::ai" });
	BOOST_TEST(static_cast<bool>(build({ "game::free" }).find_phase(ecs::phase_id{ "game::ai" })));

	scope.reset();
	const auto rebuilt = build({});
	BOOST_TEST(!rebuilt.find_phase(ecs::phase_id{ "game::ai" }));
	BOOST_TEST(rebuilt.phases.size() == ecs::builtin_phase_count);
}

BOOST_AUTO_TEST_CASE(a_system_left_on_a_removed_phase_gets_unknown_phase)
{
	{
		ecs::system_scope scope{ sf };
		scope.game_phase("game::ai", ecs::phase::update, ecs::phase_side::before);
		sf.register_system<&sys_count>("engine::on_game_phase", { .phase = "game::ai" }); // not the scope's
	}
	const auto plan = build({ "engine::on_game_phase" });
	BOOST_TEST(plan.has_diagnostic(ecs::diagnostic_kind::unknown_phase));
	BOOST_TEST(plan.system_count() == 0u);
}

BOOST_AUTO_TEST_CASE(move_assignment_releases_the_targets_own_entries)
{
	ecs::system_scope target{ sf };
	target.system<&sys_count>("game::old");
	ecs::system_scope source{ sf };
	source.system<&sys_count>("game::new");

	target = std::move(source);
	BOOST_TEST(!sf.registry().find("game::old"));
	BOOST_TEST(static_cast<bool>(sf.registry().find("game::new")));
	BOOST_TEST(target.size() == 1u);
	BOOST_TEST(source.size() == 0u);
}

BOOST_AUTO_TEST_CASE(self_move_assignment_keeps_the_entries)
{
	ecs::system_scope scope{ sf };
	scope.system<&sys_count>("game::free");
	auto& same = scope;
	scope = std::move(same);
	BOOST_TEST(scope.size() == 1u);
	BOOST_TEST(static_cast<bool>(sf.registry().find("game::free")));
}

BOOST_AUTO_TEST_CASE(an_entry_removed_behind_the_scope_is_skipped_at_release)
{
	{
		ecs::system_scope scope{ sf };
		scope.system<&sys_count>("game::free");
		BOOST_TEST(sf.unregister_system("game::free"));
		BOOST_TEST(!sf.unregister_system("game::free")); // the second removal is a no-op
	}
	BOOST_TEST(sf.registry().size() == 0u);
}

#if !SNK_ASSERTS_ENABLED
// With asserts on, registering a taken name aborts; without them the scope must not record it.
BOOST_AUTO_TEST_CASE(a_rejected_duplicate_is_not_removed_by_the_scope)
{
	sf.register_system<&sys_count>("engine::kept");
	{
		ecs::system_scope scope{ sf };
		BOOST_TEST(!scope.system<&sys_count>("engine::kept"));
		BOOST_TEST(scope.size() == 0u);
	}
	BOOST_TEST(static_cast<bool>(sf.registry().find("engine::kept")));
}
#endif

BOOST_AUTO_TEST_SUITE_END()
