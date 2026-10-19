#include <boost/test/unit_test.hpp>
#include <chrono>
#include <map>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/ecs/ecs_entity_changer.hpp>
#include <snk/ecs/ecs_random.hpp>

namespace {

	int g_ticks = 0;
	void sys_tick() { ++g_ticks; }

	std::map<const entt::registry*, float> g_seen_dt;
	std::map<const entt::registry*, float> g_seen_fixed;
	void sys_read_dt(const entt::registry& reg, const scn::delta_time& dt) { g_seen_dt[&reg] = dt.dt; }
	void sys_read_fixed(const entt::registry& reg, const scn::fixed_time& t) { g_seen_fixed[&reg] = t.dt; }

	struct context_fixture {
		context_fixture()
		{
			g_ticks = 0;
			g_seen_dt.clear();
			g_seen_fixed.clear();
			sf.register_system<&sys_tick>("tick", { .run_in_edit = false });
			sf.register_system<&sys_read_dt>("read_dt", { .run_in_edit = true });
			sf.register_system<&sys_read_fixed>("read_fixed", { .phase = ecs::phase::fixed_update });
		}

		void run_frame(const scn::world_context& ctx, ecs::run_mode mode, entt::registry& reg)
		{
			const auto order = sf.registry().phases().ordered();
			const ecs::runtime_context rt{ .current_registry = reg, .current_world_id = ctx.get_id() };
			for (size_t p = 0; p < order.size(); ++p) {
				ctx.run_phase(p, mode, rt);
			}
		}

		ecs::system_factory sf;
	};

	constexpr auto one_frame = std::chrono::duration<float>(0.001f);

}

BOOST_FIXTURE_TEST_SUITE(ScnWorldContextTests, context_fixture)

BOOST_AUTO_TEST_CASE(Create_HasIdentityEmptyStateAndNoPlan)
{
	scn::world_context ctx(3, "w3");

	BOOST_TEST(ctx.get_id() == 3u);
	BOOST_TEST(ctx.get_name() == "w3");
	BOOST_TEST(ctx.state().storage<entt::entity>().empty());
	BOOST_TEST(!ctx.systems().has_value());
	BOOST_TEST((ctx.plan() == nullptr));
	BOOST_TEST((ctx.mode() == ecs::run_mode::play));
	BOOST_TEST(!ctx.has_phase_work(0));
}

BOOST_AUTO_TEST_CASE(Seed_UsesBaseSeedAndOwnId)
{
	scn::world_context ctx(2, "w");
	ctx.seed(77);

	ecs::rng expected{ ecs::derive_seed(77, 2) };
	BOOST_TEST(ctx.state().ctx().get<ecs::rng>().next_u32() == expected.next_u32());
}

BOOST_AUTO_TEST_CASE(Frame_RunsPlannedSystemsAccordingToMode)
{
	scn::world_context ctx(0, "w");
	ctx.set_systems({ "tick" });
	ctx.rebuild_plan(sf.registry());
	ctx.resolve_slots(sf.registry().phases().ordered());
	BOOST_REQUIRE(ctx.plan() != nullptr);

	run_frame(ctx, ecs::run_mode::play, ctx.state());
	BOOST_TEST(g_ticks == 1);

	run_frame(ctx, ecs::run_mode::edit, ctx.state()); // "tick" is not run_in_edit
	BOOST_TEST(g_ticks == 1);
}

BOOST_AUTO_TEST_CASE(Frame_EmptySystemListStillBuildsAPlan)
{
	scn::world_context ctx(0, "w");
	ctx.set_systems({});
	ctx.rebuild_plan(sf.registry());

	BOOST_TEST((ctx.plan() != nullptr));
	BOOST_TEST(ctx.plan()->system_count() == 0u);
}

BOOST_AUTO_TEST_CASE(Frame_ApplyCommandsExecutesQueuedChanges)
{
	scn::world_context ctx(0, "w");
	auto& reg = ctx.state();
	const entt::entity e = reg.create();
	reg.ctx().emplace<ecs::entity_changer>().destroy(e);

	ctx.apply_commands();

	BOOST_TEST(!reg.valid(e));
}

BOOST_AUTO_TEST_CASE(Destroy_RemovedWorldLeavesNoContextBehind)
{
	scn::context_set lvl("level", nullptr);
	lvl.create_context("a", 0);
	lvl.create_context("b", 1);
	lvl.add_systems(lvl.get("b"), { "tick" }, sf);
	BOOST_REQUIRE(lvl.plan_of(1) != nullptr);

	lvl.destroy_context("b");

	BOOST_TEST(lvl.size() == 1u);
	BOOST_TEST(!lvl.has("b"));
	BOOST_TEST((lvl.find("b") == nullptr));
	BOOST_TEST((lvl.plan_of(1) == nullptr));
	BOOST_TEST(lvl.acquire_free_id() == 1u);

	g_ticks = 0;
	lvl.update(one_frame);
	BOOST_TEST(g_ticks == 0);
}

BOOST_AUTO_TEST_CASE(Level_KeepsStepperAndModeIdenticalAcrossContexts)
{
	scn::context_set lvl("level", nullptr);
	auto& a = lvl.create_context("a", 0);

	lvl.set_mode(ecs::run_mode::edit);
	auto& b = lvl.create_context("b", 1); // created later: takes the level's current mode
	BOOST_TEST((a.mode() == ecs::run_mode::edit));
	BOOST_TEST((b.mode() == ecs::run_mode::edit));

	lvl.set_mode(ecs::run_mode::play);
	lvl.update(std::chrono::duration<float>(0.01f));

	BOOST_TEST((a.mode() == ecs::run_mode::play));
	BOOST_TEST((b.mode() == ecs::run_mode::play));
	BOOST_TEST(a.stepper().alpha() == b.stepper().alpha());
	BOOST_TEST(a.stepper().alpha() > 0.0f);

	lvl.set_mode(ecs::run_mode::step);
	lvl.update(one_frame);
	BOOST_TEST((a.mode() == ecs::run_mode::pause));
	BOOST_TEST((b.mode() == ecs::run_mode::pause));

	lvl.clear();
	BOOST_TEST(lvl.size() == 0u);
}

BOOST_AUTO_TEST_CASE(Time_IsPresentInEveryContextFromCreation)
{
	scn::world_context ctx(0, "w");
	BOOST_REQUIRE(ctx.state().ctx().find<scn::delta_time>() != nullptr);
	BOOST_REQUIRE(ctx.state().ctx().find<scn::update_alpha>() != nullptr);
	BOOST_REQUIRE(ctx.state().ctx().find<scn::fixed_time>() != nullptr);
	BOOST_TEST(ctx.state().ctx().get<scn::delta_time>().dt == 0.0f);
}

BOOST_AUTO_TEST_CASE(Time_TwoContextsEachReceiveTheirOwnResources)
{
	scn::context_set lvl("level", nullptr);
	auto& a = lvl.create_context("a", 0);
	auto& b = lvl.create_context("b", 1);

	lvl.update(std::chrono::duration<float>(0.01f));

	for (auto* c : { &a, &b }) {
		const auto* dt = c->state().ctx().find<scn::delta_time>();
		const auto* alpha = c->state().ctx().find<scn::update_alpha>();
		BOOST_REQUIRE(dt && alpha);
		BOOST_TEST(dt->dt == 0.01f, boost::test_tools::tolerance(1e-6));
		BOOST_TEST(alpha->dt == a.stepper().alpha());
	}
	BOOST_TEST(a.state().ctx().find<scn::delta_time>() != b.state().ctx().find<scn::delta_time>());
}

BOOST_AUTO_TEST_CASE(Time_SystemInWorldBReadsTheDeltaOfWorldB)
{
	scn::context_set lvl("level", nullptr);
	auto& a = lvl.create_context("a", 0);
	auto& b = lvl.create_context("b", 1);
	b.set_stepper(ecs::fixed_stepper(1.0 / 60.0, 0.05)); // clamps the frame to 0.05, A keeps 0.25
	lvl.add_systems(a, { "read_dt" }, sf);
	lvl.add_systems(b, { "read_dt" }, sf);

	lvl.update(std::chrono::duration<float>(0.2f));

	BOOST_REQUIRE(g_seen_dt.size() == 2u);
	BOOST_TEST(g_seen_dt.at(&a.state()) == 0.2f, boost::test_tools::tolerance(1e-6));
	BOOST_TEST(g_seen_dt.at(&b.state()) == 0.05f, boost::test_tools::tolerance(1e-6));
}

BOOST_AUTO_TEST_CASE(Time_FixedTimeIsWrittenToEveryContextThatSteps)
{
	scn::context_set lvl("level", nullptr);
	auto& a = lvl.create_context("a", 0);
	auto& b = lvl.create_context("b", 1);
	lvl.add_systems(a, { "read_fixed" }, sf);
	lvl.add_systems(b, { "read_fixed" }, sf);

	lvl.update(std::chrono::duration<float>(0.02f));

	BOOST_REQUIRE(g_seen_fixed.size() == 2u);
	BOOST_TEST(g_seen_fixed.at(&a.state()) == 1.0f / 60.0f, boost::test_tools::tolerance(1e-6));
	BOOST_TEST(g_seen_fixed.at(&b.state()) == 1.0f / 60.0f, boost::test_tools::tolerance(1e-6));
}

BOOST_AUTO_TEST_CASE(Time_ContextCreatedLaterJoinsTheStepOfTheOthers)
{
	scn::context_set lvl("level", nullptr);
	auto& a = lvl.create_context("a", 0);
	lvl.update(std::chrono::duration<float>(0.01f)); // leaves a fraction of a step in A
	auto& b = lvl.create_context("b", 1);
	BOOST_TEST(b.stepper().alpha() == a.stepper().alpha());
	BOOST_TEST(b.stepper().alpha() > 0.0f);
}

BOOST_AUTO_TEST_SUITE_END()
