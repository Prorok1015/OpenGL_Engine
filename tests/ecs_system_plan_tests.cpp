#include <boost/test/unit_test.hpp>
#include "ecs_system.h"
#include "ecs_system_plan.hpp"
#include <entt/entt.hpp>
#include <string>
#include <vector>

// Plan building: phases, topological order with a stable tie-break, diagnostics.
// Systems are tiny loggers; their argument types play no role in the order.

namespace {

std::vector<std::string> g_log;

template <char C>
void sys() { g_log.emplace_back(1, C); }

struct stateful_log : ecs::system_interface {
	void run() { g_log.push_back("state"); }
};

struct fixture {
	fixture() { g_log.clear(); }

	template <char C>
	void add(ecs::system_desc desc = {})
	{
		sf.register_system<&sys<C>>(std::string(1, C), std::move(desc));
	}

	ecs::system_plan build(std::vector<std::string> names)
	{
		return ecs::build_system_plan(sf.registry(), names, { .validate_access = false });
	}

	void run_all(const ecs::system_plan& plan)
	{
		entt::registry reg;
		ecs::runtime_context ctx{ .get_world_registry = [&reg](size_t) -> entt::registry& { return reg; }, .current_registry = reg };
		for (const auto& phase : plan.phases) {
			for (const auto& node : phase.nodes) node(ctx);
		}
	}

	ecs::system_factory sf;
};

using names = std::vector<std::string>;

}

BOOST_FIXTURE_TEST_SUITE(EcsSystemPlanTests, fixture)

BOOST_AUTO_TEST_CASE(Diamond_IsOrderedByConstraintsWithinOnePhase)
{
	add<'d'>({ .after = { "b", "c" } });
	add<'b'>({ .after = { "a" } });
	add<'c'>({ .after = { "a" } });
	add<'a'>();

	auto plan = build({ "d", "c", "b", "a" }); // list order deliberately hostile
	BOOST_TEST(plan.names_in(ecs::phase::update) == (names{ "a", "c", "b", "d" }), boost::test_tools::per_element());
	BOOST_TEST(plan.diagnostics.empty());

	run_all(plan);
	BOOST_TEST(g_log == (names{ "a", "c", "b", "d" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Ties_AreBrokenByWorldDescListOrder)
{
	add<'x'>();
	add<'y'>();
	add<'z'>();

	BOOST_TEST(build({ "x", "y", "z" }).names_in(ecs::phase::update) == (names{ "x", "y", "z" }), boost::test_tools::per_element());
	BOOST_TEST(build({ "z", "x", "y" }).names_in(ecs::phase::update) == (names{ "z", "x", "y" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Ties_ArePartiallyConstrainedStably)
{
	add<'a'>();
	add<'b'>({ .after = { "a" } });
	add<'c'>();

	// c comes first in the list and nothing holds it back, b must wait for a.
	BOOST_TEST(build({ "c", "b", "a" }).names_in(ecs::phase::update) == (names{ "c", "a", "b" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Before_IsTheMirrorOfAfter)
{
	add<'a'>({ .before = { "b" } });
	add<'b'>();

	BOOST_TEST(build({ "b", "a" }).names_in(ecs::phase::update) == (names{ "a", "b" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Phases_RunInPhaseOrderRegardlessOfListOrder)
{
	add<'l'>({ .phase = ecs::phase::late_update });
	add<'t'>({ .phase = ecs::phase::transform });
	add<'u'>({ .phase = ecs::phase::update });
	add<'i'>({ .phase = ecs::phase::input });
	add<'f'>({ .phase = ecs::phase::fixed_update });

	auto plan = build({ "l", "t", "u", "i", "f" });
	run_all(plan);
	BOOST_TEST(g_log == (names{ "i", "f", "u", "t", "l" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(GamePhase_SitsBetweenItsAnchorAndTheNeighbour)
{
	BOOST_REQUIRE(sf.add_game_phase("ai", ecs::phase::update, ecs::phase_side::before));
	add<'u'>({ .phase = ecs::phase::update });
	add<'p'>({ .phase = ecs::phase::pre_update });
	add<'a'>({ .phase = "ai" });

	auto plan = build({ "u", "a", "p" });
	run_all(plan);
	BOOST_TEST(g_log == (names{ "p", "a", "u" }), boost::test_tools::per_element());
	BOOST_TEST(plan.phases.size() == ecs::builtin_phase_count + 1);
}

BOOST_AUTO_TEST_CASE(Cycle_IsReportedWithSystemNamesAndFallsBackToListOrder)
{
	add<'a'>({ .after = { "c" } });
	add<'b'>({ .after = { "a" } });
	add<'c'>({ .after = { "b" } });
	add<'d'>();

	auto plan = build({ "a", "b", "c", "d" });

	BOOST_REQUIRE(plan.has_diagnostic(ecs::diagnostic_kind::cycle));
	BOOST_TEST(plan.has_errors());
	const auto& msg = plan.diagnostics.front().message;
	for (const char* n : { "'update'", "a", "b", "c" }) BOOST_TEST(msg.find(n) != std::string::npos);
	BOOST_TEST(plan.names_in(ecs::phase::update) == (names{ "d", "a", "b", "c" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(TwoSystemCycle_IsDetected)
{
	add<'a'>({ .after = { "b" } });
	add<'b'>({ .after = { "a" } });

	auto plan = build({ "a", "b" });
	BOOST_TEST(plan.has_diagnostic(ecs::diagnostic_kind::cycle));
	BOOST_TEST(plan.system_count() == 2u);
}

BOOST_AUTO_TEST_CASE(SelfConstraint_IsACycle)
{
	add<'a'>({ .after = { "a" } });
	auto plan = build({ "a" });
	BOOST_TEST(plan.has_diagnostic(ecs::diagnostic_kind::cycle));
	BOOST_TEST(plan.system_count() == 1u);
}

BOOST_AUTO_TEST_CASE(UnknownSystemInList_IsDiagnosedNotFatal)
{
	add<'a'>();
	add<'b'>();

	auto plan = build({ "a", "missing", "b" });
	BOOST_TEST(plan.has_diagnostic(ecs::diagnostic_kind::unknown_system));
	BOOST_TEST(plan.diagnostics.front().message.find("missing") != std::string::npos);
	BOOST_TEST(plan.names_in(ecs::phase::update) == (names{ "a", "b" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(UnknownConstraintTarget_IsAWarningAndIgnored)
{
	add<'a'>({ .after = { "ghost" } });
	add<'b'>();

	auto plan = build({ "b", "a" });
	BOOST_TEST(plan.has_diagnostic(ecs::diagnostic_kind::unknown_constraint_target));
	BOOST_TEST(!plan.has_errors());
	BOOST_TEST(plan.diagnostics.front().message.find("ghost") != std::string::npos);
	BOOST_TEST(plan.names_in(ecs::phase::update) == (names{ "b", "a" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(ConstraintOnRegisteredButDisabledSystem_IsSilentlyIgnored)
{
	add<'a'>({ .after = { "b" } });
	add<'b'>();

	auto plan = build({ "a" }); // b exists but this world does not run it
	BOOST_TEST(plan.diagnostics.empty());
	BOOST_TEST(plan.names_in(ecs::phase::update) == (names{ "a" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(CrossPhaseConstraint_AgreeingWithPhaseOrderIsFine)
{
	add<'t'>({ .phase = ecs::phase::transform });
	add<'l'>({ .phase = ecs::phase::late_update, .after = { "t" } });

	auto plan = build({ "l", "t" });
	BOOST_TEST(plan.diagnostics.empty());
}

BOOST_AUTO_TEST_CASE(CrossPhaseConstraint_ContradictingPhaseOrderIsAnError)
{
	add<'u'>({ .phase = ecs::phase::update, .after = { "l" } });
	add<'l'>({ .phase = ecs::phase::late_update });

	auto plan = build({ "u", "l" });
	BOOST_TEST(plan.has_diagnostic(ecs::diagnostic_kind::wrong_phase_constraint));
	run_all(plan);
	BOOST_TEST(g_log == (names{ "u", "l" }), boost::test_tools::per_element()); // the phase order wins
}

BOOST_AUTO_TEST_CASE(DuplicateInList_IsDiagnosedAndRunsOnce)
{
	add<'a'>();
	auto plan = build({ "a", "a" });
	BOOST_TEST(plan.has_diagnostic(ecs::diagnostic_kind::duplicate_system));
	BOOST_TEST(plan.system_count() == 1u);
}

BOOST_AUTO_TEST_CASE(UnknownGamePhase_DropsTheSystemWithADiagnostic)
{
	add<'a'>({ .phase = "never_added" });
	auto plan = build({ "a" });
	BOOST_TEST(plan.has_diagnostic(ecs::diagnostic_kind::unknown_phase));
	BOOST_TEST(plan.system_count() == 0u);
}

BOOST_AUTO_TEST_CASE(Order_IsStableAcrossRepeatedBuilds)
{
	add<'a'>();
	add<'b'>({ .after = { "a" } });
	add<'c'>({ .after = { "a" } });
	add<'d'>({ .after = { "b", "c" } });
	add<'e'>();

	const auto first = build({ "e", "d", "c", "b", "a" }).names_in(ecs::phase::update);
	for (int i = 0; i < 20; ++i) {
		BOOST_TEST(build({ "e", "d", "c", "b", "a" }).names_in(ecs::phase::update) == first, boost::test_tools::per_element());
	}
}

BOOST_AUTO_TEST_CASE(StatefulInPlace_GetsOneInstancePerPlan)
{
	sf.register_system<&stateful_log::run, stateful_log>("s", {});
	auto p0 = build({ "s" });
	auto p1 = build({ "s" });
	auto* n0 = &p0.find_phase(ecs::phase::update)->nodes[0];
	auto* n1 = &p1.find_phase(ecs::phase::update)->nodes[0];
	BOOST_TEST((n0->instance.owner != n1->instance.owner));
	run_all(p0);
	BOOST_TEST(g_log == (names{ "state" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(RunMode_FiltersByEditFlag)
{
	using ecs::run_mode;
	BOOST_TEST(ecs::mode_runs_system(run_mode::play, false));
	BOOST_TEST(ecs::mode_runs_system(run_mode::step, false));
	BOOST_TEST(!ecs::mode_runs_system(run_mode::edit, false));
	BOOST_TEST(!ecs::mode_runs_system(run_mode::pause, false));
	BOOST_TEST(ecs::mode_runs_system(run_mode::edit, true));
	BOOST_TEST(ecs::mode_runs_system(run_mode::pause, true));
}

BOOST_AUTO_TEST_SUITE_END()
