#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_bus_context.hpp>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_scene_switch.h>
#include <snk/scene/scn_event_subscriptions.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// #564: game code replaces the level of its set with scn::request_level; the switch happens at the start of a frame,
// outside every set's update, and what the old level held is let go.

namespace {

constexpr auto one_frame = std::chrono::duration<float>(0.016f);

struct ping
{
	int value = 0;
};

// What the systems of the test levels do, set by each test.
struct orders
{
	std::optional<res::tag> from_system;
	int counted = 0;
	// A task the system starts on its set, and whether its body ran.
	ds::task_system* tasks = nullptr;
	bool start_task = false;
	bool task_ran = false;
	ds::task_handle<void> task;
};
orders* g_orders = nullptr;

void sys_request(entt::registry& reg)
{
	if (!g_orders) return;
	if (std::exchange(g_orders->start_task, false)) {
		// A task of game code that would read the world when it runs.
		g_orders->task = g_orders->tasks->run(ds::lane::compute, ds::task_desc{ .name = "level_a.task" }, scn::context_set::current()->tasks(),
			[](ds::task_context&) { g_orders->task_ran = true; });
	}
	if (g_orders->from_system) scn::request_level(reg, *std::exchange(g_orders->from_system, std::nullopt));
}

void sys_count(entt::registry&)
{
	if (g_orders) ++g_orders->counted;
}

void sys_ping(ecs::event_writer<ping>& out) { out.emit({ 1 }); }

constexpr const char* LEVEL_A = R"({"__type":"level_desc","name":"a","worlds":[
	{"__type":"world_desc","name":"a0","systems":["request","ping"],"children":{"n":{"components":{"stat":{"__type":"stat_desc","hp":1}}},
		"m":{"components":{"mesh":{"__type":"mesh_node_desc","material":"memory://t/lv_a_mat.desc"}}}}},
	{"__type":"world_desc","name":"a1","systems":[]}]})";
constexpr const char* LEVEL_B = R"({"__type":"level_desc","name":"b","worlds":[
	{"__type":"world_desc","name":"b0","systems":["count","request"],"children":{"s":{"components":{"stat":{"__type":"stat_desc","hp":2}}}}}]})";
constexpr const char* LEVEL_C = R"({"__type":"level_desc","name":"c","worlds":[
	{"__type":"world_desc","name":"c0","systems":[]}]})";

struct switch_fixture : test_util::scene_env
{
	switch_fixture()
		: hot(std::make_shared<scn::hot_reload_manager>(*assembler, *rs))
		, mgr(*rs, *assembler, sf, hot)
	{
		g_orders = &order;
		order.tasks = &tasks;
		sf.register_system<&sys_request>("request", { .phase = ecs::phase::update });
		sf.register_system<&sys_count>("count", { .phase = ecs::phase::update });
		sf.register_system<&sys_ping>("ping", { .phase = ecs::phase::update });
		mgr.add_event<ping>();
		put("lv_a_mat.desc", R"({"__type":"material_desc","queue":"opaque"})");
		level_a = put("lv_a.desc", LEVEL_A);
		level_b = put("lv_b.desc", LEVEL_B);
		level_c = put("lv_c.desc", LEVEL_C);
		BOOST_REQUIRE(mgr.load_level(level_a));
	}

	~switch_fixture() { g_orders = nullptr; }

	// One frame of the game, then what the background loads finished meanwhile.
	void frame()
	{
		mgr.update(one_frame);
		pump();
	}

	std::vector<std::string> worlds(scn::context_set& set) const
	{
		std::vector<std::string> names;
		set.for_each([&](scn::world_context& world) { names.emplace_back(world.get_name()); });
		return names;
	}

	std::vector<std::string> worlds() const { return worlds(const_cast<scn::context_manager&>(mgr).level_set()); }

	entt::registry& first_world() { return mgr.level_set().get("a0").state(); }

	bool live(std::string_view part) const
	{
		const auto tags = rs->live_resource_tags(10000);
		return std::ranges::any_of(tags, [part](const std::string& tag) { return tag.find(part) != std::string::npos; });
	}

	orders order;
	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	scn::context_manager mgr;
	res::tag level_a;
	res::tag level_b;
	res::tag level_c;
};

using names = std::vector<std::string>;

}

BOOST_FIXTURE_TEST_SUITE(ScnLevelSwitch, switch_fixture)

BOOST_AUTO_TEST_CASE(ARequestFromASystem_ReplacesTheLevelAtTheStartOfALaterFrame_OutsideEverySetsUpdate)
{
	bool switched_while_updating = true;
	mgr.set_on_level_switched([&](scn::context_set&) {
		switched_while_updating = false;
		mgr.for_each_set([&](scn::context_set& set) { switched_while_updating = switched_while_updating || set.updating(); });
	});
	order.from_system = level_b;
	frame();
	BOOST_TEST((worlds() == names{ "a0", "a1" })); // the old level goes on while the new one loads
	BOOST_TEST((scn::level_switch(first_world()).status == scn::level_switch_status::loading));
	BOOST_TEST(mgr.level_switch_pending());

	frame();
	BOOST_TEST((worlds() == names{ "b0" }));
	BOOST_TEST(!switched_while_updating);
	BOOST_TEST(order.counted == 1); // the new level ran in the frame it came in
	BOOST_TEST((mgr.level_set().tag() == level_b));
	BOOST_TEST((scn::level_switch(mgr.level_set().get("b0").state()).status == scn::level_switch_status::idle));
	BOOST_TEST(!mgr.level_switch_pending());
}

BOOST_AUTO_TEST_CASE(ARequestFromABusSubscriber_WaitsForTheNextFrame)
{
	std::optional<res::tag> next = level_b;
	auto sub = mgr.subscribe<ping>({}, [&](ecs::bus_context& ctx, const ecs::envelope<ping>&) {
		if (next) scn::request_level(ctx.registry(), *std::exchange(next, std::nullopt));
	});
	mgr.level_set().set_mode(ecs::run_mode::play);
	frame();
	BOOST_TEST((worlds() == names{ "a0", "a1" }));
	frame();
	BOOST_TEST((worlds() == names{ "b0" }));
}

BOOST_AUTO_TEST_CASE(ARequestWhileAnotherLoads_ReplacesIt_TheFirstLoadIsNotUsed)
{
	BOOST_REQUIRE(scn::request_level(first_world(), level_b));
	mgr.update(one_frame); // B is asked of the resources, not loaded yet
	BOOST_REQUIRE(scn::request_level(first_world(), level_c));
	BOOST_TEST((scn::level_switch(first_world()).level == level_c));
	pump(); // B is ready now: nothing may take it
	frame();
	BOOST_TEST((worlds() == names{ "a0", "a1" }));
	frame();
	BOOST_TEST((worlds() == names{ "c0" }));
	BOOST_TEST((mgr.level_set().tag() == level_c));
}

BOOST_AUTO_TEST_CASE(ALevelThatDoesNotLoad_KeepsTheCurrentOne_AndSaysItFailed)
{
	const res::tag missing = tag_of("missing.desc");
	BOOST_REQUIRE(scn::request_level(first_world(), missing));
	for (int i = 0; i < 3; ++i) frame();
	BOOST_TEST((worlds() == names{ "a0", "a1" }));
	const auto state = scn::level_switch(first_world());
	BOOST_TEST((state.status == scn::level_switch_status::failed));
	BOOST_TEST((state.level == missing));
	BOOST_TEST(!mgr.level_switch_pending()); // a module reload no longer waits

	BOOST_REQUIRE(scn::request_level(first_world(), level_c));
	frame();
	frame();
	BOOST_TEST((worlds() == names{ "c0" }));
}

BOOST_AUTO_TEST_CASE(TheResourcesOfTheLevelLeft_AreReleased)
{
	pump();
	// The level desc is let go once its worlds are built; what the old level still holds is in its worlds.
	BOOST_REQUIRE(live("lv_a_mat.desc")); // held by the mesh's material component
	BOOST_REQUIRE(scn::request_level(first_world(), level_b));
	// Past the grace frames of the render's material instance cache (8), which a headless test does not have.
	for (int i = 0; i < 12; ++i) frame();
	BOOST_REQUIRE((worlds() == names{ "b0" }));
	BOOST_TEST(!live("lv_a.desc"));
	BOOST_TEST(!live("lv_a_mat.desc"));
	BOOST_TEST(live("lv_b.desc"));
}

BOOST_AUTO_TEST_CASE(ATaskTheOldLevelStarted_IsStoppedBeforeItsWorldsGo)
{
	const auto held_b = require_loaded<scn::level_desc>(level_b); // ready at once: the switch comes before any pump
	order.start_task = true;
	order.from_system = level_b;
	mgr.update(one_frame); // the task is queued, B is ready
	BOOST_REQUIRE(order.task.valid());
	mgr.update(one_frame); // the switch
	BOOST_REQUIRE((worlds() == names{ "b0" }));
	pump();
	BOOST_TEST(!order.task_ran);
}

BOOST_AUTO_TEST_CASE(ARequestMadeWhileTheNewWorldsAreBuilt_IsKeptForTheNextFrame)
{
	scn::event_subscriptions observers;
	mgr.level_set().set_subscriptions(&observers);
	std::optional<res::tag> next = level_c;
	auto observer = observers.observe<test_util::stat_component>({}, scn::observed::construct, [&](ecs::bus_context& ctx, entt::entity) {
		if (next && ctx.world() == "b0") scn::request_level(ctx.registry(), *std::exchange(next, std::nullopt));
	});
	std::vector<std::string> switched;
	const engine::log_sink_handle sink = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view message) {
		if (message.find("switched to the level") != std::string_view::npos) switched.emplace_back(message);
	});
	BOOST_REQUIRE(scn::request_level(first_world(), level_b));
	frame();
	frame();
	BOOST_REQUIRE((worlds() == names{ "b0" }));
	BOOST_TEST((mgr.level_set().tag() == level_b));
	BOOST_REQUIRE(switched.size() == 1u);
	BOOST_TEST(switched.front().find("lv_b.desc") != std::string::npos); // the level built, not the one asked for during it
	BOOST_TEST((scn::level_switch(mgr.level_set().get("b0").state()).level == level_c));
	frame();
	frame();
	BOOST_TEST((worlds() == names{ "c0" }));
	mgr.level_set().clear();
	mgr.level_set().set_subscriptions(nullptr);
}

BOOST_AUTO_TEST_CASE(ASetOfPlayWorlds_KeepsTheirKind_AndTheLevelIsNotTouched)
{
	auto& play = mgr.create_set("play", nullptr, scn::set_options{ .hot_reload = false });
	play.load_from_desc(*require_loaded<scn::level_desc>(level_a), sf, *assembler, scn::context_kind::play);
	BOOST_REQUIRE(scn::request_level(play.get("a0").state(), level_c));
	frame();
	frame();
	BOOST_TEST((worlds(play) == names{ "c0" }));
	BOOST_TEST((play.get("c0").kind() == scn::context_kind::play));
	BOOST_TEST((worlds() == names{ "a0", "a1" }));
	BOOST_TEST((mgr.level_set().tag() == level_a));
}

BOOST_AUTO_TEST_CASE(TheLevelSetCanBeClosedToRequests_AsInTheEditor_ARequestIsRefusedAtOnce)
{
	mgr.set_level_set_switchable(false);
	std::size_t warnings = 0;
	const engine::log_sink_handle sink = engine::add_log_sink([&](std::string_view, engine::log_level level, std::string_view message) {
		if (level == engine::log_level::warn && message.find("takes no level switch") != std::string_view::npos) ++warnings;
	});
	// A system asking every frame: refused each time, said once, nothing ever waits for it.
	for (int i = 0; i < 3; ++i) {
		BOOST_TEST(!scn::request_level(first_world(), level_b));
		frame();
		BOOST_TEST(!mgr.level_switch_pending());
	}
	BOOST_TEST(warnings == 1u);
	BOOST_TEST((worlds() == names{ "a0", "a1" }));
	BOOST_TEST((scn::level_switch(first_world()).status == scn::level_switch_status::idle));

	mgr.set_level_set_switchable(true);
	BOOST_REQUIRE(scn::request_level(first_world(), level_c));
	frame();
	frame();
	BOOST_TEST((worlds() == names{ "c0" }));
}

BOOST_AUTO_TEST_CASE(ALoadOfTheLevel_DropsARequestOfTheLevelBefore)
{
	BOOST_REQUIRE(scn::request_level(first_world(), level_b));
	BOOST_REQUIRE(mgr.load_level(level_c));
	frame();
	frame();
	BOOST_TEST((worlds() == names{ "c0" }));
	BOOST_TEST(!mgr.level_switch_pending());
}

// A world made while running, and every world after a frame of its set, knows its set outside the set's update too
// (a task's continuation on the main thread).
BOOST_AUTO_TEST_CASE(AWorldMadeWhileRunning_CanAskOutsideTheFrame_AndTheLevelsKindIsKept)
{
	auto& late = mgr.create_context(scn::context_manager::level_set_name, "late", scn::context_kind::runtime);
	BOOST_REQUIRE(scn::request_level(late.state(), level_b));
	frame();
	frame();
	BOOST_TEST((worlds() == names{ "b0" }));
	BOOST_TEST((mgr.level_set().get("b0").kind() == scn::context_kind::level));
	BOOST_REQUIRE(scn::request_level(mgr.level_set().get("b0").state(), level_c));
	frame();
	frame();
	BOOST_TEST((worlds() == names{ "c0" }));
}

BOOST_AUTO_TEST_CASE(AWorldOfNoSet_IsRefused)
{
	entt::registry alone;
	BOOST_TEST(!scn::request_level(alone, level_b));
	BOOST_TEST((scn::level_switch(alone).status == scn::level_switch_status::idle));
}

BOOST_AUTO_TEST_SUITE_END()
