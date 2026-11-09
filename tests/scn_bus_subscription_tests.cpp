#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_bus_context.hpp>
#include <snk/ecs/ecs_channel.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/scn_event_subscriptions.h>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <vector>

// Bus subscriptions (#603 PR 3, WORLD_BUS.md 3.4): heard in every matching world of the sets they cover, also worlds
// made later; called during the delivery with an ecs::bus_context; what they emit goes to the next delivery, what
// they change lands right after the delivery, their mail goes through the set's channel.

namespace {

struct ping
{
	int value = 0;
};

struct pong
{
	int value = 0;
};

struct mail
{
	int value = 0;
};

struct marker
{
	int value = 0;
};

constexpr auto one_frame = std::chrono::duration<float>(0.001f);

struct emitter_state
{
	int next = 0;
};

void sys_emit(ecs::event_writer<ping>& out, emitter_state& state) { out.emit({ state.next++ }); }

struct post_seen
{
	std::vector<int> markers;
	std::vector<int> pongs;
	std::vector<int> mail;
};

// After the delivery point: sees what the subscribers changed in the same frame.
void sys_post(entt::view<entt::get_t<marker>> view, const ecs::event_reader<pong>& pongs, const ecs::inbox<mail>& in, post_seen& seen)
{
	for (auto&& [entity, m] : view.each()) seen.markers.push_back(m.value);
	for (const auto& e : pongs.events()) seen.pongs.push_back(e.payload.value);
	for (const auto& m : in.messages()) seen.mail.push_back(m.payload.value);
}

struct subscription_fixture : test_util::scene_env
{
	subscription_fixture()
		: mgr(*rs, *assembler, sf)
	{
		sf.register_system<&sys_emit>("emit", { .phase = ecs::phase::update });
		sf.register_system<&sys_post>("post", { .phase = ecs::phase::post_update });
		mgr.add_event<ping>();
		mgr.add_event<pong>();
		mgr.channels().add<mail>();
	}

	scn::world_context& world(std::string_view set_name, std::string_view name, std::vector<std::string> systems = { "emit" })
	{
		auto& set = mgr.find_set(set_name) ? *mgr.find_set(set_name) : mgr.create_set(set_name);
		auto& context = set.create_context(name, static_cast<uint32_t>(set.size()));
		set.add_systems(context, systems, sf);
		return context;
	}

	void frame() { mgr.update(one_frame); }

	ecs::system_factory sf;
	scn::context_manager mgr;
};

using lines = std::vector<std::string>;

} // namespace

BOOST_FIXTURE_TEST_SUITE(ScnBusSubscriptionTests, subscription_fixture)

BOOST_AUTO_TEST_CASE(ASubscriberGetsTheEventsOfItsWorldsWithTheirSetAndWorld)
{
	world("a", "w");
	world("b", "w");
	lines heard;
	auto sub = mgr.subscribe<ping>({}, [&](ecs::bus_context& ctx, const ecs::envelope<ping>& e) {
		heard.push_back(std::format("{}/{}:{}#{}", ctx.set(), ctx.world(), e.payload.value, e.seq));
	});
	frame();
	frame();
	BOOST_TEST((heard == lines{ "a/w:0#0", "b/w:0#0", "a/w:1#0", "b/w:1#0" }));
}

BOOST_AUTO_TEST_CASE(TheFilterNamesASetAWorldOrBoth)
{
	world("a", "x");
	world("a", "y");
	world("b", "x");
	lines heard;
	auto record = [&](std::string tag) {
		return [&heard, tag](ecs::bus_context& ctx, const ecs::envelope<ping>&) { heard.push_back(std::format("{} {}/{}", tag, ctx.set(), ctx.world())); };
	};
	auto by_set = mgr.subscribe<ping>({ .set = "a" }, record("set"));
	auto by_world = mgr.subscribe<ping>({ .world = "x" }, record("world"));
	auto by_both = mgr.subscribe<ping>({ .set = "b", .world = "x" }, record("both"));
	frame();
	BOOST_TEST((heard == lines{ "set a/x", "world a/x", "set a/y", "world b/x", "both b/x" }));
}

BOOST_AUTO_TEST_CASE(AWorldMadeAfterTheSubscriptionHearsIt)
{
	int calls = 0;
	auto sub = mgr.subscribe<ping>({ .set = "s" }, [&](ecs::bus_context&, const ecs::envelope<ping>&) { ++calls; });
	world("s", "w");
	frame();
	BOOST_TEST(calls == 1);
	world("s", "later");
	frame();
	BOOST_TEST(calls == 3);
}

BOOST_AUTO_TEST_CASE(ASubscriptionMadeDuringADeliveryFiresFromTheNextOne)
{
	world("s", "w");
	lines heard;
	std::optional<ecs::connection> late;
	auto first = mgr.subscribe<ping>({}, [&](ecs::bus_context&, const ecs::envelope<ping>& e) {
		heard.push_back(std::format("first:{}", e.payload.value));
		if (!late) late = mgr.subscribe<ping>({}, [&](ecs::bus_context&, const ecs::envelope<ping>& l) { heard.push_back(std::format("late:{}", l.payload.value)); });
	});
	frame();
	BOOST_TEST((heard == lines{ "first:0" }));
	frame();
	BOOST_TEST((heard == lines{ "first:0", "first:1", "late:1" }));
}

BOOST_AUTO_TEST_CASE(DisconnectingSilencesEveryWorld_AlsoFromInsideTheCallback)
{
	world("s", "a");
	world("s", "b");
	int calls = 0;
	std::optional<ecs::connection> sub;
	sub = mgr.subscribe<ping>({}, [&](ecs::bus_context&, const ecs::envelope<ping>&) {
		++calls;
		sub->disconnect();
	});
	frame();
	frame();
	BOOST_TEST(calls == 1); // world b, later in the same frame, no longer hears it either
	BOOST_TEST(mgr.find_set("s")->get("a").state().ctx().get<ecs::world_bus>().deliveries() == 2u);
}

BOOST_AUTO_TEST_CASE(WhatASubscriberChangesIsVisibleAfterTheDeliveryInTheSameFrame)
{
	auto& w = world("s", "w", { "emit", "post" });
	const entt::entity target = w.state().create();
	bool had_marker_in_callback = true;
	auto sub = mgr.subscribe<ping>({}, [&](ecs::bus_context& ctx, const ecs::envelope<ping>& e) {
		had_marker_in_callback = ctx.registry().all_of<marker>(target);
		ctx.changer().emplace<marker>(target, marker{ 10 + e.payload.value });
	});
	frame();
	BOOST_TEST(!had_marker_in_callback); // queued, applied once the subscribers are done
	BOOST_TEST((w.state().ctx().get<post_seen>().markers == std::vector<int>{ 10 }));
}

BOOST_AUTO_TEST_CASE(WhatASubscriberEmitsComesInTheNextDelivery)
{
	auto& w = world("s", "w", { "emit", "post" });
	auto sub = mgr.subscribe<ping>({}, [](ecs::bus_context& ctx, const ecs::envelope<ping>& e) { ctx.writer<pong>().emit({ 100 + e.payload.value }); });
	frame();
	BOOST_TEST(w.state().ctx().get<post_seen>().pongs.empty());
	frame();
	BOOST_TEST((w.state().ctx().get<post_seen>().pongs == std::vector<int>{ 100 }));
}

BOOST_AUTO_TEST_CASE(ASubscriberMailsAnotherWorldOfTheSet)
{
	world("s", "sender");
	auto& receiver = world("s", "receiver", { "post" });
	auto sub = mgr.subscribe<ping>({ .world = "sender" }, [](ecs::bus_context& ctx, const ecs::envelope<ping>& e) {
		ctx.outbox<mail>().send({ "receiver" }, mail{ e.payload.value + 1 });
	});
	frame();
	frame();
	// Sent during frame 0's delivery, exchanged at the start of frame 1.
	BOOST_TEST((receiver.state().ctx().get<post_seen>().mail == std::vector<int>{ 1 }));
}

BOOST_AUTO_TEST_CASE(ASubscriptionSurvivesALevelReload)
{
	const auto level = put("lv.desc", R"({"__type":"level_desc","name":"lv","worlds":[{"__type":"world_desc","name":"w0","systems":["emit"]}]})");
	int calls = 0;
	auto sub = mgr.subscribe<ping>({ .set = "level", .world = "w0" }, [&](ecs::bus_context&, const ecs::envelope<ping>&) { ++calls; });
	BOOST_REQUIRE(mgr.load_level(level));
	frame();
	BOOST_REQUIRE(mgr.load_level(level));
	frame();
	BOOST_TEST(calls == 2);
}

BOOST_AUTO_TEST_CASE(ALayersSubscriptionsHearOnlyItsSets_AndGoWithTheLayer)
{
	world("base", "w");
	auto& layer = mgr.create_layer();
	auto& layer_set = mgr.create_set("game", &layer);
	auto& game_world = layer_set.create_context("w", 0);
	layer_set.add_systems(game_world, { "emit" }, sf);

	int calls = 0;
	auto sub = layer.subscriptions().subscribe<ping>({}, [&](ecs::bus_context& ctx, const ecs::envelope<ping>&) {
		BOOST_TEST(ctx.set() == "game");
		++calls;
	});
	frame();
	BOOST_TEST(calls == 1);
	BOOST_TEST(layer.subscriptions().size() == 1u);

	mgr.destroy_layer(layer);
	frame();
	BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_CASE(AWorldWhoseBusDoesNotKnowTheTypeIsSkipped)
{
	scn::event_subscriptions subs;
	int calls = 0;
	auto sub = subs.subscribe<marker>({}, [&](ecs::bus_context&, const ecs::envelope<marker>&) { ++calls; });
	scn::context_set set("s", nullptr);
	set.set_subscriptions(&subs);
	auto& w = set.create_context("w", 0);
	set.update(one_frame);
	BOOST_TEST(calls == 0);
	BOOST_TEST(!w.state().ctx().contains<ecs::event_writer<marker>>());
}

BOOST_AUTO_TEST_CASE(StepDeliversOncePerStep_AndAppliesWhatSubscribersChange)
{
	auto& w = world("s", "w", { "post" });
	const entt::entity target = w.state().create();
	int calls = 0;
	auto sub = mgr.subscribe<ping>({}, [&](ecs::bus_context& ctx, const ecs::envelope<ping>& e) {
		if (calls++ == 0) ctx.changer().emplace<marker>(target, marker{ 10 + e.payload.value });
	});
	mgr.set_mode("s", ecs::run_mode::pause);
	mgr.emit("s", "w", ping{ 1 });
	frame();
	frame();
	BOOST_TEST(calls == 0);

	mgr.step("s", 1);
	frame();
	BOOST_TEST(calls == 1);
	BOOST_TEST((w.state().ctx().get<post_seen>().markers == std::vector<int>{ 11 }));
	frame(); // Pause again
	BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_CASE(InEditASubscriberIsNotCalled)
{
	world("s", "w", {});
	int calls = 0;
	auto sub = mgr.subscribe<ping>({}, [&](ecs::bus_context&, const ecs::envelope<ping>&) { ++calls; });
	mgr.set_mode("s", ecs::run_mode::edit);
	mgr.emit("s", "w", ping{ 1 });
	frame();
	mgr.set_mode("s", ecs::run_mode::play);
	frame();
	BOOST_TEST(calls == 0); // dropped in Edit, nothing left to deliver in Play
}

BOOST_AUTO_TEST_CASE(ASetAndWorldFilterTellsThePlayTwinFromTheLevelWorld)
{
	world("level", "w0");
	world("play", "w0");
	lines heard;
	auto sub = mgr.subscribe<ping>({ .set = "play", .world = "w0" }, [&](ecs::bus_context& ctx, const ecs::envelope<ping>&) {
		heard.push_back(std::format("{}/{}", ctx.set(), ctx.world()));
	});
	frame();
	BOOST_TEST((heard == lines{ "play/w0" }));
}

BOOST_AUTO_TEST_CASE(SubscribingAndDisconnectingAgainAndAgainLeavesNothingInTheWorld)
{
	auto& w = world("s", "w");
	int calls = 0;
	for (int i = 0; i < 50; ++i) {
		auto c = mgr.subscribe<ping>({}, [&](ecs::bus_context&, const ecs::envelope<ping>&) { ++calls; });
		frame();
		BOOST_TEST(scn::event_subscriptions::connected_in(w.state()) == 1u);
		c.disconnect();
	}
	frame();
	BOOST_TEST(scn::event_subscriptions::connected_in(w.state()) == 0u);
	BOOST_TEST(calls == 50);
}

BOOST_AUTO_TEST_CASE(SubscribersSendingBackAndForthStayWithinTheLimit)
{
	auto& w = world("s", "w", {});
	auto& bus = w.state().ctx().get<ecs::world_bus>();
	bus.set_event_limit(8);
	auto sub = mgr.subscribe<ping>({}, [](ecs::bus_context& ctx, const ecs::envelope<ping>& e) {
		ctx.writer<ping>().emit({ e.payload.value + 1 });
		ctx.writer<ping>().emit({ e.payload.value + 1 });
	});
	mgr.emit("s", "w", ping{ 0 });
	for (int i = 0; i < 10; ++i) {
		frame();
		BOOST_TEST(bus.queued() <= 8u);
	}
	BOOST_TEST(w.state().ctx().get<ecs::event_reader<ping>>().events().size() == 8u);
}

BOOST_AUTO_TEST_CASE(ASubscriptionSkippedForAnUnknownTypeIsConnectedOnceTheTypeIsThere)
{
	scn::event_subscriptions subs;
	int calls = 0;
	auto sub = subs.subscribe<marker>({}, [&](ecs::bus_context&, const ecs::envelope<marker>&) { ++calls; });
	scn::context_set set("s", nullptr);
	set.set_subscriptions(&subs);
	auto& w = set.create_context("w", 0);
	set.update(one_frame);
	BOOST_TEST(scn::event_subscriptions::connected_in(w.state()) == 0u);

	set.event_types().add<marker>();
	set.update(one_frame);
	BOOST_TEST(scn::event_subscriptions::connected_in(w.state()) == 1u);
	set.post_event("w", marker{ 1 });
	set.update(one_frame);
	BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_SUITE_END()
