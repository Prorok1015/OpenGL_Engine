#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include "level/scn_level.h"
#include "level/scn_level_desc.h"
#include "scn_ecs_assembler.h"
#include "ecs_random.hpp"
#include "ecs_entity_changer.hpp"
#include "scn_model.h"
#include "scn_transform_system.h"
#include "scn_animation_job.h"
#include "scn_camera_controller_system.h"
#include "inp_ecs_input_manager.h"
#include "desc_system.h"
#include "res_system.h"

namespace json = boost::json;

namespace {

	std::vector<std::string> g_log;

	// Order comes from the phase and after/before declared at registration, never from argument types.
	void sys_a() { g_log.emplace_back("A"); }
	void sys_b() { g_log.emplace_back("B"); }
	void sys_c() { g_log.emplace_back("C"); }
	void sys_d() { g_log.emplace_back("D"); }
	void sys_w() { g_log.emplace_back("W"); }
	void sys_fixed() { g_log.emplace_back("F"); }
	void sys_game() { g_log.emplace_back("G"); }
	void sys_edit() { g_log.emplace_back("E"); }

	std::vector<uint32_t> g_rolls;
	void sys_roll(ecs::rng& rng) { g_rolls.push_back(rng.next_u32()); }

	std::vector<float> g_dts;
	void sys_capture_dt(scn::level_res<const scn::delta_time> dt) { g_dts.push_back(dt->dt); }

	struct stateful_sys : ecs::system_interface {
		explicit stateful_sys(std::string tag) : tag(std::move(tag)) {}
		void run() { g_log.push_back(tag); }
		std::string tag;
	};

	// Counts its own runs, so a re-created instance shows up as the count starting over.
	struct counting_sys : ecs::system_interface {
		explicit counting_sys(std::string tag) : tag(std::move(tag)) {}
		void run() { g_log.push_back(tag + std::to_string(++runs)); }
		std::string tag;
		int runs = 0;
	};

	struct fixture {
		fixture()
			: ds(rs)
			, assembler(ds, rs)
		{
			g_log.clear();
			g_dts.clear();
			g_rolls.clear();
			sf.register_system<&sys_roll>("roll");
			sf.register_system<&sys_fixed>("fixed", { .phase = ecs::phase::fixed_update });
			sf.register_system<&sys_game>("game");
			sf.register_system<&sys_edit>("edit", { .run_in_edit = true });
			sf.register_system<&sys_capture_dt>("dt", { .run_in_edit = true });
			sf.register_system<&sys_a>("a");
			sf.register_system<&sys_b>("b", { .after = { "a" } });
			sf.register_system<&sys_c>("c", { .after = { "a" } });
			sf.register_system<&sys_d>("d", { .after = { "b", "c" } });
			sf.register_system<&sys_w>("w");
			sf.register_system<&stateful_sys::run, stateful_sys>("stateful", {}, std::string{ "S" });
		}

		scn::world_desc make_world_desc(const std::string& name, const std::vector<std::string>& systems)
		{
			json::object obj;
			obj["name"] = name;
			obj["systems"] = json::value_from(systems);
			scn::world_desc d;
			d.deserialize(ds, obj);
			return d;
		}

		size_t count(const std::string& s) const { return (size_t)std::ranges::count(g_log, s); }
		size_t pos(const std::string& s) const
		{
			return (size_t)std::distance(g_log.begin(), std::ranges::find(g_log, s));
		}

		res::resource_system rs;
		desc::desc_system ds;
		scn::ecs_assembler assembler;
		ecs::system_factory sf;
	};

	constexpr auto one_frame = std::chrono::duration<float>(0.001f); // below the fixed step

	using names = std::vector<std::string>;

}

BOOST_FIXTURE_TEST_SUITE(ScnLevelSchedulerTests, fixture)

BOOST_AUTO_TEST_CASE(Diamond_EachSystemRunsOnceAndInOrder)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "d", "c", "b", "a" }, sf);

	lvl.update(one_frame);

	BOOST_TEST(g_log.size() == 4u);
	BOOST_TEST(count("A") == 1u);
	BOOST_TEST(count("B") == 1u);
	BOOST_TEST(count("C") == 1u);
	BOOST_TEST(count("D") == 1u);
	BOOST_TEST(pos("A") < pos("B"));
	BOOST_TEST(pos("A") < pos("C"));
	BOOST_TEST(pos("B") < pos("D"));
	BOOST_TEST(pos("C") < pos("D"));
}

BOOST_AUTO_TEST_CASE(Diamond_OrderIsStableAcrossFramesAndLevels)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "a", "b", "c", "d" }, sf);

	lvl.update(one_frame);
	auto first = g_log;
	g_log.clear();
	lvl.update(one_frame);
	BOOST_TEST(g_log == first);

	scn::level other(nullptr);
	auto& ow = other.create_world("w0", 0);
	other.add_world_systems(ow, { "a", "b", "c", "d" }, sf);
	g_log.clear();
	other.update(one_frame);
	BOOST_TEST(g_log == first);
}

BOOST_AUTO_TEST_CASE(Plan_IsBuiltOnceNotEveryFrame)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "stateful", "a" }, sf);

	const ecs::system_plan* plan = lvl.plan_of(0);
	BOOST_REQUIRE(plan != nullptr);
	void* self_before = plan->find_phase(ecs::phase::update)->nodes[0].instance.self;

	for (int i = 0; i < 5; ++i) lvl.update(one_frame);

	BOOST_TEST((lvl.plan_of(0) == plan));
	BOOST_TEST((plan->find_phase(ecs::phase::update)->nodes[0].instance.self == self_before));
	BOOST_TEST(count("S") == 5u);
}

BOOST_AUTO_TEST_CASE(PlanOrder_FollowsConstraintsThenDescListOrder)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "d", "b", "c", "a" }, sf);

	BOOST_TEST(lvl.plan_of(0)->names_in(ecs::phase::update) == (names{ "a", "b", "c", "d" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(ReloadWorld_KeepsOtherWorldsSystems)
{
	scn::level lvl(nullptr);
	auto& wa = lvl.create_world("wa", 0);
	lvl.add_world_systems(wa, { "w" }, sf);
	auto& wb = lvl.create_world("wb", 1);
	lvl.add_world_systems(wb, { "stateful" }, sf);

	lvl.update(one_frame);
	BOOST_TEST(count("W") == 1u);
	BOOST_TEST(count("S") == 1u);

	g_log.clear();
	const ecs::system_plan* plan_a = lvl.plan_of(0);
	auto desc_b = make_world_desc("wb", { "stateful" });
	lvl.reload_world("wb", desc_b, sf, assembler);
	lvl.update(one_frame);

	BOOST_TEST(count("W") == 1u);
	BOOST_TEST(count("S") == 1u);
	BOOST_TEST(g_log.size() == 2u);
	BOOST_TEST((lvl.plan_of(0) == plan_a)); // the other world's plan was not touched
}

BOOST_AUTO_TEST_CASE(LoadClearLoad_DoesNotDuplicateSystems)
{
	scn::level lvl(nullptr);
	{
		auto& w = lvl.create_world("w0", 0);
		lvl.add_world_systems(w, { "a", "b", "c", "d" }, sf);
	}
	lvl.update(one_frame);
	BOOST_TEST(g_log.size() == 4u);

	lvl.clear();
	g_log.clear();
	lvl.update(one_frame);
	BOOST_TEST(g_log.empty());

	{
		auto& w = lvl.create_world("w0", 0);
		lvl.add_world_systems(w, { "a", "b", "c", "d" }, sf);
	}
	lvl.update(one_frame);

	BOOST_TEST(g_log.size() == 4u);
	BOOST_TEST(count("A") == 1u);
	BOOST_TEST(count("D") == 1u);
}

BOOST_AUTO_TEST_CASE(MoveAndSwap_LevelStillRunsSystems)
{
	scn::level a(nullptr);
	{
		auto& w = a.create_world("w0", 0);
		a.add_world_systems(w, { "a", "b", "c", "d" }, sf);
	}

	scn::level moved(std::move(a));
	moved.update(one_frame);
	BOOST_TEST(g_log.size() == 4u);

	scn::level other(nullptr);
	{
		auto& w = other.create_world("ow", 0);
		other.add_world_systems(w, { "stateful" }, sf);
	}
	g_log.clear();
	moved.swap(other);

	moved.update(one_frame);
	BOOST_TEST(g_log == (names{ "S" }));

	g_log.clear();
	other.update(one_frame);
	BOOST_TEST(g_log.size() == 4u);
}

BOOST_AUTO_TEST_CASE(RemoveWorld_DropsItsSystemsKeepsOthers)
{
	scn::level lvl(nullptr);
	auto& wa = lvl.create_world("wa", 0);
	lvl.add_world_systems(wa, { "w" }, sf);
	auto& wb = lvl.create_world("wb", 1);
	lvl.add_world_systems(wb, { "stateful" }, sf);

	lvl.remove_world("wb");
	lvl.update(one_frame);

	BOOST_TEST(g_log == (names{ "W" }));
	BOOST_TEST(lvl.plan_of(1) == nullptr);
}

BOOST_AUTO_TEST_CASE(UnknownSystemInWorld_IsDiagnosedAndTheRestStillRuns)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "a", "not_registered", "b" }, sf);

	BOOST_TEST(lvl.plan_of(0)->has_diagnostic(ecs::diagnostic_kind::unknown_system));
	lvl.update(one_frame);
	BOOST_TEST(g_log == (names{ "A", "B" }));
}

BOOST_AUTO_TEST_CASE(Worlds_RunInIdOrderEveryPhase)
{
	scn::level lvl(nullptr);
	auto& w1 = lvl.create_world("second", 1);
	lvl.add_world_systems(w1, { "stateful" }, sf);
	auto& w0 = lvl.create_world("first", 0);
	lvl.add_world_systems(w0, { "w" }, sf);

	lvl.update(one_frame);
	BOOST_TEST(g_log == (names{ "W", "S" }));
}

// Bug #79: a graph edge appeared between same-named stateless systems of different worlds
// because every world fed one shared organizer. Plans are per world, so the worlds cannot
// influence each other.
BOOST_AUTO_TEST_CASE(SameStatelessSystemInTwoWorlds_PlansAreIndependent)
{
	scn::level lvl(nullptr);
	auto& w0 = lvl.create_world("w0", 0);
	lvl.add_world_systems(w0, { "a", "b", "c", "d" }, sf);
	auto& w1 = lvl.create_world("w1", 1);
	lvl.add_world_systems(w1, { "d", "a" }, sf); // same systems, different subset and order

	BOOST_TEST(lvl.plan_of(0)->names_in(ecs::phase::update) == (names{ "a", "b", "c", "d" }), boost::test_tools::per_element());
	BOOST_TEST(lvl.plan_of(1)->names_in(ecs::phase::update) == (names{ "d", "a" }), boost::test_tools::per_element());
	BOOST_TEST(lvl.plan_of(0)->diagnostics.empty());
	BOOST_TEST(lvl.plan_of(1)->diagnostics.empty());

	lvl.update(one_frame);
	BOOST_TEST(g_log == (names{ "A", "B", "C", "D", "D", "A" }), boost::test_tools::per_element());

	// changing one world's list leaves the other's plan alone
	const ecs::system_plan* before = lvl.plan_of(0);
	lvl.add_world_systems(w1, { "a" }, sf);
	BOOST_TEST((lvl.plan_of(0) == before));
	g_log.clear();
	lvl.update(one_frame);
	BOOST_TEST(g_log == (names{ "A", "B", "C", "D", "A" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(AcquireFreeWorldId_ReusesGapInsteadOfCount)
{
	scn::level lvl(nullptr);
	lvl.create_world("w0", 0);
	lvl.create_world("w1", 1);
	lvl.create_world("w2", 2);
	BOOST_TEST(lvl.acquire_free_world_id() == 3u);

	// Reloading keeps ids; ids 0 and 2 alive with 1 missing must yield 1, not get_world_count() == 2.
	scn::level gap(nullptr);
	gap.create_world("a", 0);
	gap.create_world("c", 2);
	BOOST_TEST(gap.get_world_count() == 2u);
	BOOST_TEST(gap.acquire_free_world_id() == 1u);
}

BOOST_AUTO_TEST_CASE(CreateWorld_DuplicateNameIsRejectedAndLeavesNoStaleEntries)
{
	scn::level lvl(nullptr);
	auto& first = lvl.create_world("w0", 0);
	lvl.add_world_systems(first, { "a" }, sf);

	auto& again = lvl.create_world("w0", 5);

	BOOST_TEST(&again == &first);
	BOOST_TEST(lvl.get_world_count() == 1u);
	BOOST_TEST(&lvl.get_world("w0") == &first);
	BOOST_TEST(lvl.has_world("w0"));
	BOOST_TEST(lvl.plan_of(5) == nullptr);
	BOOST_TEST(lvl.plan_of(0) != nullptr);

	g_log.clear();
	lvl.update(one_frame); // would touch freed memory if the old world had been replaced
	BOOST_TEST(count("A") == 1u);
}

// ARCHITECTURE_REVIEW 9.5: the system list of editor_level.desc, spread over phases by the
// production registrations. This is the table from the design document.
BOOST_AUTO_TEST_CASE(EditorLevelDesc_IsLaidOutAsTheDesignTable)
{
	ecs::system_factory prod;
	scn::init_transform_system(prod);
	scn::init_animation_system(prod);
	scn::init_mouse_controller_system(prod);
	inp::register_ecs_input_system(prod, std::make_shared<inp::ecs_input_manager>());

	const std::vector<std::string> editor_level = {
		"inp::update_input_state", "scn::animation_controller_update", "scn::animation_system_node",
		"scn::depth_system", "scn::transform_system", "scn::animation_system_matrix",
		"scn::update_camera_matrix_system" };

	auto check = [&](const std::vector<std::string>& list) {
		auto plan = ecs::build_system_plan(prod.registry(), list, { .validate_access = false });
		BOOST_TEST(plan.diagnostics.empty());
		BOOST_TEST(plan.names_in(ecs::phase::input) == (names{ "inp::update_input_state" }), boost::test_tools::per_element());
		BOOST_TEST(plan.names_in(ecs::phase::update) == (names{ "scn::animation_controller_update", "scn::animation_system_node" }), boost::test_tools::per_element());
		BOOST_TEST(plan.names_in(ecs::phase::transform) == (names{ "scn::depth_system", "scn::transform_system" }), boost::test_tools::per_element());
		BOOST_TEST(plan.names_in(ecs::phase::late_update) == (names{ "scn::animation_system_matrix" }), boost::test_tools::per_element());
		BOOST_TEST(plan.names_in(ecs::phase::pre_update) == (names{ "scn::update_camera_matrix_system" }), boost::test_tools::per_element());
		BOOST_TEST(plan.names_in(ecs::phase::fixed_update).empty());
		BOOST_TEST(plan.names_in(ecs::phase::post_update).empty());
	};

	check(editor_level);
	// the order no longer depends on how the desc happens to list the systems
	check(std::vector<std::string>(editor_level.rbegin(), editor_level.rend()));
}

// ---------------------------------------------------------------- fixed_update

BOOST_AUTO_TEST_CASE(FixedUpdate_StepsFollowTheAccumulator)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "fixed" }, sf);

	std::vector<size_t> per_frame;
	for (float dt : { 0.04f, 0.04f, 0.04f, 0.001f }) {
		g_log.clear();
		lvl.update(std::chrono::duration<float>(dt));
		per_frame.push_back(g_log.size());
	}
	BOOST_TEST(per_frame == (std::vector<size_t>{ 2, 2, 3, 0 }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(FixedUpdate_SameDeltasGiveTheSameRunSequence)
{
	auto run = [&] {
		g_log.clear();
		scn::level lvl(nullptr);
		auto& w = lvl.create_world("w0", 0);
		lvl.add_world_systems(w, { "game", "fixed" }, sf);
		for (float dt : { 0.03f, 0.02f, 0.05f, 0.016f, 0.2f }) lvl.update(std::chrono::duration<float>(dt));
		return g_log;
	};
	BOOST_TEST(run() == run(), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(FixedUpdate_RunsBeforeUpdateAndOncePerStep)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "game", "fixed" }, sf);

	lvl.update(std::chrono::duration<float>(0.04f)); // two steps
	BOOST_TEST(g_log == (names{ "F", "F", "G" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(FixedUpdate_HugeFrameIsClampedAndStepCountIsCapped)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "fixed", "dt" }, sf);

	lvl.update(std::chrono::duration<float>(30.0f));
	BOOST_TEST(count("F") == 8u);
	BOOST_REQUIRE(g_dts.size() == 1u);
	BOOST_TEST(g_dts[0] == 0.25f); // delta_time is the clamped value
}

// ---------------------------------------------------------------- Edit / Play / Pause / Step

BOOST_AUTO_TEST_CASE(Mode_DefaultsToPlayAndRunsEverything)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "game", "edit", "fixed" }, sf);

	BOOST_TEST((lvl.mode() == ecs::run_mode::play));
	lvl.update(std::chrono::duration<float>(1.0f / 60.0f));
	BOOST_TEST(g_log == (names{ "F", "G", "E" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Mode_EditRunsOnlyRunInEditSystems)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "game", "edit", "fixed" }, sf);
	lvl.set_mode(ecs::run_mode::edit);

	lvl.update(std::chrono::duration<float>(1.0f / 30.0f));
	BOOST_TEST(g_log == (names{ "E" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Mode_PauseFreezesTimeAndRunsOnlyEditSystems)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "game", "edit", "fixed", "dt" }, sf);
	lvl.set_mode(ecs::run_mode::pause);

	lvl.update(std::chrono::duration<float>(0.5f));
	BOOST_TEST(g_log == (names{ "E" }), boost::test_tools::per_element());
	BOOST_REQUIRE(g_dts.size() == 1u);
	BOOST_TEST(g_dts[0] == 0.0f);

	// resuming does not replay the paused time
	g_log.clear();
	lvl.set_mode(ecs::run_mode::play);
	lvl.update(std::chrono::duration<float>(0.001f));
	BOOST_TEST(count("F") == 0u);
}

BOOST_AUTO_TEST_CASE(Mode_StepRunsExactlyOneFrameThenPauses)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "game", "edit", "fixed", "dt" }, sf);
	lvl.set_mode(ecs::run_mode::step);

	lvl.update(std::chrono::duration<float>(5.0f)); // the real dt is irrelevant for a step
	BOOST_TEST(g_log == (names{ "F", "G", "E" }), boost::test_tools::per_element());
	BOOST_REQUIRE(g_dts.size() == 1u);
	BOOST_TEST(g_dts[0] == 1.0f / 60.0f, boost::test_tools::tolerance(1e-5));
	BOOST_TEST((lvl.mode() == ecs::run_mode::pause));

	g_log.clear();
	lvl.update(std::chrono::duration<float>(1.0f));
	BOOST_TEST(g_log == (names{ "E" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Mode_SurvivesClearAndReload)
{
	scn::level lvl(nullptr);
	lvl.set_mode(ecs::run_mode::edit);
	lvl.clear();
	BOOST_TEST((lvl.mode() == ecs::run_mode::edit));
}

// ---------------------------------------------------------------- seeded rng resource

BOOST_AUTO_TEST_CASE(Rng_SameSeedReplaysTheSameSequenceThroughTheResource)
{
	auto run = [&](uint64_t seed) {
		g_rolls.clear();
		scn::level lvl(nullptr);
		lvl.set_seed(seed);
		auto& w = lvl.create_world("w0", 0);
		lvl.add_world_systems(w, { "roll" }, sf);
		for (int i = 0; i < 5; ++i) lvl.update(one_frame);
		return g_rolls;
	};

	const auto a = run(42);
	BOOST_REQUIRE(a.size() == 5u);
	BOOST_TEST(run(42) == a, boost::test_tools::per_element());
	BOOST_TEST(run(43) != a);
}

BOOST_AUTO_TEST_CASE(Rng_WorldsGetIndependentStreams)
{
	scn::level lvl(nullptr);
	auto& w0 = lvl.create_world("w0", 0);
	lvl.add_world_systems(w0, { "roll" }, sf);
	auto& w1 = lvl.create_world("w1", 1);
	lvl.add_world_systems(w1, { "roll" }, sf);

	lvl.update(one_frame);
	BOOST_REQUIRE(g_rolls.size() == 2u);
	BOOST_TEST(g_rolls[0] != g_rolls[1]);
}

BOOST_AUTO_TEST_CASE(Rng_SetSeedReseedsExistingWorlds)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "roll" }, sf);

	lvl.set_seed(7);
	lvl.update(one_frame);
	lvl.update(one_frame);
	lvl.set_seed(7);
	lvl.update(one_frame);

	BOOST_REQUIRE(g_rolls.size() == 3u);
	BOOST_TEST(g_rolls[2] == g_rolls[0]); // the sequence restarted
	BOOST_TEST(lvl.seed() == 7u);
}

// ---------------------------------------------------------------- phases by id, plan rebuild (#211)

// A game phase added after the first world's plan was built: the two plans have different phase
// lists, and every system must still run in its own phase, fixed_update only through the stepper.
BOOST_AUTO_TEST_CASE(GamePhaseAddedBetweenPlans_EachSystemRunsInItsOwnPhase)
{
	auto tagged = [&](const char* name, const char* tag, ecs::phase_id phase) {
		sf.register_system<&stateful_sys::run, stateful_sys>(name, { .phase = phase }, std::string{ tag });
	};
	tagged("f0", "F0", ecs::phase::fixed_update);
	tagged("f1", "F1", ecs::phase::fixed_update);
	tagged("u0", "U0", ecs::phase::update);
	tagged("u1", "U1", ecs::phase::update);
	tagged("early1", "P1", "early");

	scn::level lvl(nullptr);
	auto& w0 = lvl.create_world("w0", 0);
	lvl.add_world_systems(w0, { "f0", "u0" }, sf);
	BOOST_REQUIRE(sf.add_game_phase("early", ecs::phase::fixed_update, ecs::phase_side::before));
	auto& w1 = lvl.create_world("w1", 1);
	lvl.add_world_systems(w1, { "f1", "u1", "early1" }, sf);

	lvl.update(std::chrono::duration<float>(0.04f)); // two fixed steps
	BOOST_TEST(g_log == (names{ "P1", "F0", "F1", "F0", "F1", "U0", "U1" }), boost::test_tools::per_element());

	g_log.clear();
	lvl.update(one_frame); // no step: the fixed systems stay quiet, the game phase runs once
	BOOST_TEST(g_log == (names{ "P1", "U0", "U1" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(RebuildAllPlans_PicksUpAGamePhaseAddedAfterLoad)
{
	sf.register_system<&sys_game>("late_game", { .phase = "late_phase" });

	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "a", "late_game" }, sf);
	BOOST_TEST(lvl.plan_of(0)->has_diagnostic(ecs::diagnostic_kind::unknown_phase));
	lvl.update(one_frame);
	BOOST_TEST(g_log == (names{ "A" }), boost::test_tools::per_element());

	BOOST_REQUIRE(sf.add_game_phase("late_phase", ecs::phase::update, ecs::phase_side::after));
	lvl.rebuild_all_plans(sf);
	BOOST_TEST(!lvl.plan_of(0)->has_diagnostic(ecs::diagnostic_kind::unknown_phase));

	g_log.clear();
	lvl.update(one_frame);
	BOOST_TEST(g_log == (names{ "A", "G" }), boost::test_tools::per_element());
}

// ---------------------------------------------------------------- command buffers

BOOST_AUTO_TEST_CASE(WorldWithoutSystems_StillGetsItsCommandsApplied)
{
	scn::level lvl(nullptr);
	auto& empty = lvl.create_world("empty", 0);
	auto& reg = empty.state();
	const entt::entity e = reg.create();
	reg.ctx().emplace<ecs::entity_changer>().destroy(e);
	BOOST_REQUIRE(lvl.plan_of(0) == nullptr);

	lvl.update(one_frame);
	BOOST_TEST(!reg.valid(e));
}

BOOST_AUTO_TEST_CASE(LevelState_GetsItsCommandsApplied)
{
	scn::level lvl(nullptr);
	auto& reg = lvl.state();
	const entt::entity e = reg.create();
	reg.ctx().emplace<ecs::entity_changer>().destroy(e);

	lvl.update(one_frame);
	BOOST_TEST(!reg.valid(e));
}

// ---------------------------------------------------------------- fixed accumulator on load / clear

BOOST_AUTO_TEST_CASE(LoadFromDesc_ResetsTheFixedAccumulator)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "fixed" }, sf);
	lvl.update(std::chrono::duration<float>(0.01f)); // 0.01 of a 1/60 step left in the accumulator
	BOOST_TEST(count("F") == 0u);

	json::object obj;
	obj["name"] = "empty";
	obj["worlds"] = json::array{};
	scn::level_desc level_desc;
	level_desc.deserialize(ds, obj);
	lvl.load_from_desc(level_desc, sf, assembler);

	lvl.update(std::chrono::duration<float>(0.01f)); // 0.02 would be a step; the leftover must be gone
	BOOST_TEST(count("F") == 0u);
}

BOOST_AUTO_TEST_CASE(Clear_ResetsTheFixedAccumulator)
{
	scn::level lvl(nullptr);
	lvl.update(std::chrono::duration<float>(0.01f));
	lvl.clear();

	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "fixed" }, sf);
	lvl.update(std::chrono::duration<float>(0.01f));
	BOOST_TEST(count("F") == 0u);
}

// ---------------------------------------------------------------- reload contract (systems only)

BOOST_AUTO_TEST_CASE(SystemsReload_RecreatesPerWorldInstancesKeepsSharedOnes)
{
	sf.register_system<&counting_sys::run, counting_sys>("counter_per_world", {}, std::string{ "P" });
	sf.register_system<&counting_sys::run, counting_sys>("counter_shared", {}, std::make_shared<counting_sys>(std::string{ "H" }));

	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "counter_per_world", "counter_shared" }, sf);
	lvl.update(one_frame);
	lvl.update(one_frame);
	BOOST_TEST(g_log == (names{ "P1", "H1", "P2", "H2" }), boost::test_tools::per_element());

	g_log.clear();
	lvl.rebuild_all_plans(sf); // the plan rebuild a systems-only reload does
	lvl.update(one_frame);
	BOOST_TEST(g_log == (names{ "P1", "H3" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(SystemsReload_KeepsModeSeedAndWorldRng)
{
	scn::level lvl(nullptr);
	lvl.set_seed(11);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "roll" }, sf);
	lvl.update(one_frame); // advances the world's stream
	lvl.set_mode(ecs::run_mode::edit);
	ecs::rng expected = w.state().ctx().get<ecs::rng>();

	lvl.rebuild_all_plans(sf);

	BOOST_TEST(lvl.seed() == 11u);
	BOOST_TEST((lvl.mode() == ecs::run_mode::edit));
	BOOST_TEST(w.state().ctx().get<ecs::rng>().next_u32() == expected.next_u32()); // not re-seeded
}

BOOST_AUTO_TEST_SUITE_END()
