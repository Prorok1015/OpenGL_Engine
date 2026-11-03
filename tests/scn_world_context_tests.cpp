#include <boost/test/unit_test.hpp>
#include <chrono>
#include <limits>
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

// ---------------------------------------------------------------- step(n) and time scale (US-63-A, #489)

BOOST_AUTO_TEST_CASE(Step_RunsExactlyNFramesOfOneFixedStepThenPauses)
{
	scn::context_set set("play", nullptr);
	auto& a = set.create_context("a", 0, scn::context_kind::play);
	set.add_systems(a, { "tick", "read_dt", "read_fixed" }, sf);

	set.set_mode(ecs::run_mode::pause);
	set.update(std::chrono::duration<float>(0.2f));
	BOOST_TEST(g_ticks == 0); // gameplay does not run in Pause
	BOOST_TEST(g_seen_dt.at(&a.state()) == 0.0f);

	set.step(3); // from Pause
	BOOST_TEST((set.mode() == ecs::run_mode::step));
	BOOST_TEST(set.pending_steps() == 3u);
	for (int frame = 1; frame <= 3; ++frame) {
		g_seen_fixed.clear();
		set.update(std::chrono::duration<float>(0.2f)); // the wall dt does not matter
		BOOST_TEST(g_ticks == frame);
		BOOST_TEST(g_seen_dt.at(&a.state()) == 1.0f / 60.0f, boost::test_tools::tolerance(1e-6f));
		BOOST_TEST(a.fixed_steps() == 1); // Play would run 8 for 0.2 s
		BOOST_TEST(g_seen_fixed.at(&a.state()) == 1.0f / 60.0f, boost::test_tools::tolerance(1e-6f));
		BOOST_TEST(set.pending_steps() == static_cast<uint32_t>(3 - frame));
	}
	BOOST_TEST((set.mode() == ecs::run_mode::pause));
	BOOST_TEST((a.mode() == ecs::run_mode::pause));

	set.update(std::chrono::duration<float>(0.2f));
	set.update(std::chrono::duration<float>(0.2f));
	BOOST_TEST(g_ticks == 3);
	BOOST_TEST(g_seen_dt.at(&a.state()) == 0.0f);
}

BOOST_AUTO_TEST_CASE(Step_SetModeStepIsOneFrameAndAnotherModeCancelsTheSteps)
{
	scn::context_set set("play", nullptr);
	auto& a = set.create_context("a", 0, scn::context_kind::play);
	set.add_systems(a, { "tick" }, sf);

	set.set_mode(ecs::run_mode::step);
	BOOST_TEST(set.pending_steps() == 1u);
	set.update(one_frame);
	BOOST_TEST(g_ticks == 1);
	BOOST_TEST((set.mode() == ecs::run_mode::pause));
	BOOST_TEST(set.pending_steps() == 0u);

	set.step(5);
	set.update(one_frame);
	set.set_mode(ecs::run_mode::pause);
	BOOST_TEST(set.pending_steps() == 0u);
	set.update(one_frame);
	BOOST_TEST(g_ticks == 2);
}

BOOST_AUTO_TEST_CASE(FramesRun_CountsPlayAndStepFramesOnly)
{
	scn::context_set set("play", nullptr);
	set.create_context("a", 0, scn::context_kind::play);
	BOOST_TEST(set.frames_run() == 0u);

	set.set_mode(ecs::run_mode::play);
	for (int i = 0; i < 3; ++i) set.update(one_frame);
	BOOST_TEST(set.frames_run() == 3u);

	set.set_mode(ecs::run_mode::pause);
	set.update(one_frame);
	set.update(one_frame);
	BOOST_TEST(set.frames_run() == 3u);

	set.step(2);
	for (int i = 0; i < 4; ++i) set.update(one_frame); // two frames of the step, then paused again
	BOOST_TEST(set.frames_run() == 5u);

	set.set_mode(ecs::run_mode::edit);
	set.update(one_frame);
	BOOST_TEST(set.frames_run() == 5u);
}

BOOST_AUTO_TEST_CASE(TimeScale_MultipliesTheFrameDeltaOfAPlayContext)
{
	scn::context_set set("play", nullptr);
	auto& a = set.create_context("a", 0, scn::context_kind::play);
	set.add_systems(a, { "read_dt" }, sf);
	set.set_mode(ecs::run_mode::play);
	BOOST_TEST(set.time_scale() == 1.0f);

	for (const float scale : { 0.5f, 0.0f, 2.0f }) {
		BOOST_TEST_CONTEXT("scale " << scale) {
			set.set_time_scale(scale);
			BOOST_TEST(a.time_scale() == scale);
			set.update(std::chrono::duration<float>(0.01f));
			BOOST_TEST(g_seen_dt.at(&a.state()) == 0.01f * scale, boost::test_tools::tolerance(1e-6f));
		}
	}

	// scaled before the clamp: 0.2 * 2 is clamped to max_frame_dt (0.25)
	set.update(std::chrono::duration<float>(0.2f));
	BOOST_TEST(g_seen_dt.at(&a.state()) == 0.25f, boost::test_tools::tolerance(1e-6f));

	// a context created later takes the scale of its set
	BOOST_TEST(set.create_context("b", 1, scn::context_kind::play).time_scale() == 2.0f);
}

BOOST_AUTO_TEST_CASE(TimeScale_ZeroAdvancesNoFixedStepAndAStepIgnoresTheScale)
{
	scn::context_set set("play", nullptr);
	auto& a = set.create_context("a", 0, scn::context_kind::play);
	set.add_systems(a, { "read_dt", "read_fixed" }, sf);

	set.set_time_scale(0.0f);
	set.update(std::chrono::duration<float>(0.1f));
	BOOST_TEST(a.fixed_steps() == 0);
	BOOST_TEST(g_seen_fixed.empty());

	set.set_time_scale(4.0f);
	set.step(1);
	set.update(std::chrono::duration<float>(0.01f));
	BOOST_TEST(g_seen_dt.at(&a.state()) == 1.0f / 60.0f, boost::test_tools::tolerance(1e-6f));
	BOOST_TEST(a.fixed_steps() == 1);
}

BOOST_AUTO_TEST_CASE(TimeScale_NegativeOrNaNIsRefusedAsZero)
{
	scn::context_set set("play", nullptr);
	auto& a = set.create_context("a", 0, scn::context_kind::play);
	for (const float bad : { -1.0f, -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() }) {
		set.set_time_scale(1.0f);
		set.set_time_scale(bad);
		BOOST_TEST(set.time_scale() == 0.0f);
		BOOST_TEST(a.time_scale() == 0.0f);
	}
}

BOOST_AUTO_TEST_CASE(TimeScale_AboveTheMaximumIsClampedToIt)
{
	scn::context_set set("play", nullptr);
	auto& a = set.create_context("a", 0, scn::context_kind::play);
	set.add_systems(a, { "read_dt" }, sf);

	set.set_time_scale(scn::context_set::max_time_scale);
	BOOST_TEST(set.time_scale() == scn::context_set::max_time_scale); // the maximum itself is valid
	for (const float big : { scn::context_set::max_time_scale + 1.0f, 100.0f, std::numeric_limits<float>::infinity() }) {
		set.set_time_scale(1.0f);
		set.set_time_scale(big);
		BOOST_TEST(set.time_scale() == scn::context_set::max_time_scale);
		BOOST_TEST(a.time_scale() == scn::context_set::max_time_scale);
	}

	// at the maximum a 60 fps frame still fits in the stepper's step budget: no simulated time is dropped
	set.update(std::chrono::duration<float>(1.0f / 60.0f));
	BOOST_TEST(a.fixed_steps() <= a.stepper().max_steps());
	BOOST_TEST(g_seen_dt.at(&a.state()) == 8.0f / 60.0f, boost::test_tools::tolerance(1e-5f));
}

BOOST_AUTO_TEST_CASE(Step_ZeroFramesIsRefusedAndRunsOne)
{
	scn::context_set set("play", nullptr);
	auto& a = set.create_context("a", 0, scn::context_kind::play);
	set.add_systems(a, { "tick" }, sf);
	set.set_mode(ecs::run_mode::pause);

	set.step(0);
	BOOST_TEST((set.mode() == ecs::run_mode::step));
	BOOST_TEST(set.pending_steps() == 1u);
	set.update(one_frame);
	set.update(one_frame);
	BOOST_TEST(g_ticks == 1);
	BOOST_TEST((set.mode() == ecs::run_mode::pause));
}

BOOST_AUTO_TEST_SUITE_END()
