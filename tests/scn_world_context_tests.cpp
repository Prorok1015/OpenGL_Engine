#include <boost/test/unit_test.hpp>
#include <chrono>
#include <snk/scene/level/scn_level.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/ecs/ecs_entity_changer.hpp>
#include <snk/ecs/ecs_random.hpp>

namespace {

	int g_ticks = 0;
	void sys_tick() { ++g_ticks; }

	struct context_fixture {
		context_fixture()
		{
			g_ticks = 0;
			sf.register_system<&sys_tick>("tick", { .run_in_edit = false });
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
	scn::level lvl(nullptr);
	lvl.create_world("a", 0);
	lvl.create_world("b", 1);
	lvl.add_world_systems(lvl.get_world("b"), { "tick" }, sf);
	BOOST_REQUIRE(lvl.plan_of(1) != nullptr);

	lvl.remove_world("b");

	BOOST_TEST(lvl.get_world_count() == 1u);
	BOOST_TEST(!lvl.has_world("b"));
	BOOST_TEST((lvl.find_world("b") == nullptr));
	BOOST_TEST((lvl.plan_of(1) == nullptr));
	BOOST_TEST(lvl.acquire_free_world_id() == 1u);

	g_ticks = 0;
	lvl.update(one_frame);
	BOOST_TEST(g_ticks == 0);
}

BOOST_AUTO_TEST_CASE(Level_KeepsStepperAndModeIdenticalAcrossContexts)
{
	scn::level lvl(nullptr);
	auto& a = lvl.create_world("a", 0);

	lvl.set_mode(ecs::run_mode::edit);
	auto& b = lvl.create_world("b", 1); // created later: takes the level's current mode
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
	BOOST_TEST(lvl.get_world_count() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
