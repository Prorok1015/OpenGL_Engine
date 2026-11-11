#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/scn_animation.h>
#include <snk/scene/scn_animation_collection_desc.h>
#include <snk/scene/scn_animation_job.h>
#include <snk/scene/scn_model.h>
#include <chrono>
#include <limits>
#include <memory>
#include <type_traits>
#include <string>
#include <vector>

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

// The end of a clip without loop on the world bus (#563): scn::animation_finished.
namespace {

// The layout is part of the module API (#563): a field added, removed or retyped shows here.
struct animation_finished_layout
{
	entt::entity entity;
	std::string clip;
};
static_assert(std::is_aggregate_v<scn::animation_finished>);
static_assert(sizeof(scn::animation_finished) == sizeof(animation_finished_layout));
static_assert(alignof(scn::animation_finished) == alignof(animation_finished_layout));
static_assert(std::is_same_v<decltype(scn::animation_finished::entity), entt::entity>);
static_assert(std::is_same_v<decltype(scn::animation_finished::clip), std::string>);

struct heard_finish
{
	entt::entity entity = entt::null;
	std::string clip;
	bool alive = false;
};

std::vector<heard_finish> g_heard;

// After the delivery point: it gets the clip's end the frame it happens.
void hear_finished(const ecs::event_reader<scn::animation_finished>& in, entt::registry& reg)
{
	for (const auto& e : in.events()) g_heard.push_back({ e.payload.entity, e.payload.clip, reg.valid(e.payload.entity) });
}

// Destroys a model whose clip just stopped, in the frame it stops, before the delivery.
void destroy_stopped(entt::registry& reg, entt::view<entt::get_t<scn::animation_controller_component>> models)
{
	std::vector<entt::entity> stopped;
	for (auto [model, controller] : models.each()) {
		if (!controller.playing && !controller.current_animation.empty()) stopped.push_back(model);
	}
	for (const entt::entity model : stopped) reg.destroy(model);
}

struct finish_fixture : test_util::scene_env
{
	finish_fixture() : events(make_events()), set("play", nullptr, nullptr, {}, events)
	{
		g_heard.clear();
		scn::init_animation_system(sf);
		sf.register_system<&hear_finished>("test::hear_finished", { .phase = ecs::phase::post_update });
		sf.register_system<&destroy_stopped>("test::destroy_stopped",
			{ .phase = ecs::phase::update, .after = { "scn::animation_controller_update" } });
	}

	static std::shared_ptr<ecs::bus_registry> make_events()
	{
		auto out = std::make_shared<ecs::bus_registry>();
		out->add<scn::animation_finished>();
		return out;
	}

	entt::entity add_model(std::vector<std::string> systems)
	{
		auto& world = set.create_context("w", 0);
		set.add_systems(world, systems, sf);
		reg = &world.state();
		const entt::entity model = reg->create();
		scn::animation_collection_component collection;
		collection.clips = load<scn::animation_collection_desc>(put("clips.desc", CLIPS))->clips;
		reg->emplace<scn::animation_collection_component>(model, std::move(collection));
		reg->emplace<scn::animation_controller_component>(model);
		return model;
	}

	void frame(float seconds) { set.update(std::chrono::duration<float>(seconds)); }

	std::shared_ptr<ecs::bus_registry> events;
	ecs::system_factory sf;
	scn::context_set set;
	entt::registry* reg = nullptr;
};

const std::vector<std::string> k_hearing{ "scn::animation_controller_update", "test::hear_finished" };

}

BOOST_FIXTURE_TEST_SUITE(ScnAnimationFinishedTests, finish_fixture)

BOOST_AUTO_TEST_CASE(AClipWithoutLoopTellsItsEndOnceTheFrameItEnds)
{
	const entt::entity model = add_model(k_hearing);
	BOOST_REQUIRE(scn::play_animation(*reg, model, "jump", false)); // 0.5 s
	frame(0.2f);
	frame(0.2f);
	BOOST_TEST(g_heard.empty());

	frame(0.2f);
	BOOST_REQUIRE(g_heard.size() == 1u);
	BOOST_TEST((g_heard[0].entity == model));
	BOOST_TEST(g_heard[0].clip == "jump");
	BOOST_TEST(g_heard[0].alive);
	BOOST_TEST(!reg->get<scn::animation_controller_component>(model).playing);

	frame(0.2f);
	frame(0.2f);
	BOOST_TEST(g_heard.size() == 1u);
}

BOOST_AUTO_TEST_CASE(TheSameClipPlayedAgainAfterItsEndTellsItsEndAgain)
{
	const entt::entity model = add_model(k_hearing);
	BOOST_REQUIRE(scn::play_animation(*reg, model, "jump", false));
	for (int i = 0; i < 3; ++i) frame(0.2f);
	BOOST_REQUIRE(g_heard.size() == 1u);

	BOOST_REQUIRE(scn::play_animation(*reg, model, "jump", false));
	frame(0.2f);
	frame(0.2f);
	BOOST_TEST(g_heard.size() == 1u);
	frame(0.2f);
	BOOST_REQUIRE(g_heard.size() == 2u);
	BOOST_TEST(g_heard[1].clip == "jump");
	BOOST_TEST((g_heard[1].entity == model));
}

BOOST_AUTO_TEST_CASE(ALoopGoingRoundTellsNothing)
{
	const entt::entity model = add_model(k_hearing);
	BOOST_REQUIRE(scn::play_animation(*reg, model, "walk")); // 0.5 s, loop
	for (int i = 0; i < 6; ++i) frame(0.2f);
	BOOST_TEST(g_heard.empty());
	BOOST_TEST(reg->get<scn::animation_controller_component>(model).playing);
}

BOOST_AUTO_TEST_CASE(AClipReplacedOrStoppedBeforeItsEndTellsNothing)
{
	const entt::entity model = add_model(k_hearing);
	BOOST_REQUIRE(scn::play_animation(*reg, model, "jump", false));
	frame(0.2f);
	BOOST_REQUIRE(scn::play_animation(*reg, model, "walk"));
	for (int i = 0; i < 4; ++i) frame(0.2f);
	BOOST_TEST(g_heard.empty());

	BOOST_REQUIRE(scn::play_animation(*reg, model, "jump", false));
	frame(0.2f);
	reg->get<scn::animation_controller_component>(model).playing = false;
	for (int i = 0; i < 4; ++i) frame(0.2f);
	BOOST_TEST(g_heard.empty());

	BOOST_REQUIRE(scn::play_animation(*reg, model, "jump", false, 0.0f));
	for (int i = 0; i < 4; ++i) frame(0.2f);
	BOOST_TEST(g_heard.empty());
}

BOOST_AUTO_TEST_CASE(AModelDestroyedInTheFrameItsClipEnds_IsHeardAsGone)
{
	const entt::entity model = add_model({ "scn::animation_controller_update", "test::destroy_stopped", "test::hear_finished" });
	BOOST_REQUIRE(scn::play_animation(*reg, model, "jump", false));
	for (int i = 0; i < 3; ++i) frame(0.2f);

	BOOST_REQUIRE(g_heard.size() == 1u);
	BOOST_TEST(g_heard[0].clip == "jump");
	BOOST_TEST(!g_heard[0].alive);
	BOOST_TEST(!reg->valid(model));
}

BOOST_AUTO_TEST_CASE(InEditTheEndIsNotKeptForLater)
{
	const entt::entity model = add_model(k_hearing);
	set.set_mode(ecs::run_mode::edit);
	BOOST_REQUIRE(scn::play_animation(*reg, model, "jump", false));
	for (int i = 0; i < 4; ++i) frame(0.2f); // the controller runs in Edit, the bus drops what it emits
	BOOST_REQUIRE(!reg->get<scn::animation_controller_component>(model).playing);

	set.set_mode(ecs::run_mode::play);
	frame(0.2f);
	BOOST_TEST(g_heard.empty());
}

BOOST_AUTO_TEST_SUITE_END()
