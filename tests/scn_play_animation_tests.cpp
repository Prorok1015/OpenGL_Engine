#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/ecs/ecs_system.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/scn_animation.h>
#include <snk/scene/scn_animation_collection_desc.h>
#include <snk/scene/scn_animation_job.h>
#include <snk/scene/scn_model.h>
#include <chrono>
#include <limits>
#include <string>

// play_animation (#563): game code switches the clip of a model with one call.

namespace {

// idle 2 s (20 ticks at 10), walk 0.5 s (10 ticks at 20), jump 0.5 s (6 ticks at 12)
constexpr const char* CLIPS = R"({"__type":"animation_collection_desc","clips":[
	{"__type":"animation_clip_desc","name":"idle","duration":20.0,"ticks_per_second":10.0},
	{"__type":"animation_clip_desc","name":"walk","duration":10.0,"ticks_per_second":20.0},
	{"__type":"animation_clip_desc","name":"jump","duration":6.0,"ticks_per_second":12.0}]})";

struct play_fixture : test_util::scene_env
{
	play_fixture() : lvl("level", nullptr)
	{
		scn::init_animation_system(sf);
		auto& world = lvl.create_context("w", 0);
		lvl.add_systems(world, { "scn::animation_controller_update" }, sf);
		reg = &world.state();

		model = reg->create();
		scn::animation_collection_component collection;
		collection.clips = load<scn::animation_collection_desc>(put("clips.desc", CLIPS))->clips;
		reg->emplace<scn::animation_collection_component>(model, std::move(collection));
		reg->emplace<scn::animation_controller_component>(model);
	}

	scn::animation_controller_component& controller() { return reg->get<scn::animation_controller_component>(model); }
	void frame(float seconds) { lvl.update(std::chrono::duration<float>(seconds)); }

	ecs::system_factory sf;
	scn::context_set lvl;
	entt::registry* reg = nullptr;
	entt::entity model = entt::null;
};

}

BOOST_FIXTURE_TEST_SUITE(ScnPlayAnimationTests, play_fixture)

BOOST_AUTO_TEST_CASE(OneCallSwitchesIdleWalkAndJump)
{
	BOOST_REQUIRE(scn::play_animation(*reg, model, "idle"));
	BOOST_TEST(controller().current_animation == "idle");
	BOOST_TEST(controller().duration == 20.0f);
	BOOST_TEST(controller().ticks_per_second == 10.0f);
	BOOST_TEST(controller().playing);
	BOOST_TEST(controller().loop);
	BOOST_TEST(controller().speed == 1.0f);
	frame(0.1f);
	BOOST_TEST(controller().current_time > 0.0f);

	BOOST_REQUIRE(scn::play_animation(*reg, model, "walk", true, 2.0f));
	BOOST_TEST(controller().current_animation == "walk");
	BOOST_TEST(controller().duration == 10.0f);
	BOOST_TEST(controller().ticks_per_second == 20.0f);
	BOOST_TEST(controller().current_time == 0.0f);
	BOOST_TEST(controller().speed == 2.0f);

	BOOST_REQUIRE(scn::play_animation(*reg, model, "jump", false));
	BOOST_TEST(controller().current_animation == "jump");
	BOOST_TEST(controller().duration == 6.0f);
	BOOST_TEST(controller().ticks_per_second == 12.0f);
	BOOST_TEST(controller().current_time == 0.0f);
	BOOST_TEST(!controller().loop);
	BOOST_TEST(controller().speed == 1.0f);
}

BOOST_AUTO_TEST_CASE(TheClipThatPlaysKeepsItsTimeAndTakesTheNewLoopAndSpeed)
{
	BOOST_REQUIRE(scn::play_animation(*reg, model, "idle"));
	frame(0.2f);
	const float time = controller().current_time;
	BOOST_REQUIRE(time > 0.0f);

	BOOST_REQUIRE(scn::play_animation(*reg, model, "idle", false, 0.5f));
	BOOST_TEST(controller().current_time == time);
	BOOST_TEST(!controller().loop);
	BOOST_TEST(controller().speed == 0.5f);
	BOOST_TEST(controller().playing);
}

BOOST_AUTO_TEST_CASE(TheClipThatPlaysIsNotLookedUpAgain)
{
	BOOST_REQUIRE(scn::play_animation(*reg, model, "idle"));
	// what the collection would give back is not written again on the continuing call
	controller().duration = 99.0f;
	controller().ticks_per_second = 3.0f;
	BOOST_REQUIRE(scn::play_animation(*reg, model, "idle", true, 2.0f));
	BOOST_TEST(controller().duration == 99.0f);
	BOOST_TEST(controller().ticks_per_second == 3.0f);
	BOOST_TEST(controller().speed == 2.0f);
}

BOOST_AUTO_TEST_CASE(AClipWithoutLoopStopsAtItsEndAndTheNextCallPlaysItAgain)
{
	BOOST_REQUIRE(scn::play_animation(*reg, model, "jump", false));
	for (int i = 0; i < 4; ++i) frame(0.2f); // 0.8 s > 0.5 s of the clip
	BOOST_TEST(!controller().playing);
	BOOST_TEST(controller().current_time == 0.5f, boost::test_tools::tolerance(1e-5f));

	BOOST_REQUIRE(scn::play_animation(*reg, model, "jump", false));
	BOOST_TEST(controller().playing);
	BOOST_TEST(controller().current_time == 0.0f);
}

BOOST_AUTO_TEST_CASE(SpeedZeroHoldsThePose)
{
	BOOST_REQUIRE(scn::play_animation(*reg, model, "walk", true, 0.0f));
	frame(0.1f);
	BOOST_TEST(controller().current_time == 0.0f);
	BOOST_TEST(controller().playing);
}

BOOST_AUTO_TEST_CASE(AFailedCallLeavesTheControllerAsItWas)
{
	BOOST_REQUIRE(scn::play_animation(*reg, model, "walk", true, 1.5f));
	frame(0.1f);
	const scn::animation_controller_component before = controller();
	const auto unchanged = [&] {
		const auto& now = controller();
		return now.current_animation == before.current_animation && now.current_time == before.current_time
			&& now.speed == before.speed && now.playing == before.playing && now.loop == before.loop
			&& now.duration == before.duration && now.ticks_per_second == before.ticks_per_second;
	};

	BOOST_TEST(!scn::play_animation(*reg, model, "run"));
	BOOST_TEST(unchanged());
	BOOST_TEST(!scn::play_animation(*reg, model, "idle", true, -1.0f));
	BOOST_TEST(unchanged());
	BOOST_TEST(!scn::play_animation(*reg, model, "idle", true, std::numeric_limits<float>::quiet_NaN()));
	BOOST_TEST(unchanged());
	BOOST_TEST(!scn::play_animation(*reg, model, "idle", true, std::numeric_limits<float>::infinity()));
	BOOST_TEST(unchanged());
}

BOOST_AUTO_TEST_CASE(OnlyTheEntityThatHoldsTheControllerPlays)
{
	// a node under the model: no walk up to the root
	const entt::entity node = reg->create();
	reg->emplace<scn::parent_component>(node, model);
	BOOST_TEST(!scn::play_animation(*reg, node, "walk"));
	BOOST_TEST(!scn::play_animation(*reg, entt::null, "walk"));

	// a controller without a collection
	const entt::entity bare = reg->create();
	reg->emplace<scn::animation_controller_component>(bare);
	BOOST_TEST(!scn::play_animation(*reg, bare, "walk"));
	BOOST_TEST(reg->get<scn::animation_controller_component>(bare).current_animation.empty());
	BOOST_TEST(!reg->get<scn::animation_controller_component>(bare).playing);
}

BOOST_AUTO_TEST_SUITE_END()
