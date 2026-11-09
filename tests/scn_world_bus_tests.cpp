#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

// The world bus in a context set (#603 PR 2, WORLD_BUS.md 3.2, 3.5): the delivery point right after the builtin
// update phase, the reader window per mode, external emits, late senders, event types of a layer. Systems take the
// bus as plain parameters (ecs::event_writer<T>&, const ecs::event_reader<T>&), as production systems will.

namespace {

struct ping
{
	int value = 0;
};

struct layer_event
{
	int value = 0;
};

struct registered_later
{
	int value = 0;
};

constexpr auto one_frame = std::chrono::duration<float>(0.001f);

struct emitter_state
{
	int next = 0;
};

// What one reader saw, call by call: "d<delivery>:[values]".
template <int N>
struct seen_log
{
	std::vector<std::string> calls;
};

template <int N>
void record(const ecs::event_reader<ping>& in, seen_log<N>& log)
{
	std::string line = std::format("d{}:[", in.delivery());
	for (const auto& e : in.events()) line += std::format("{}{}", line.back() == '[' ? "" : ",", e.payload.value);
	log.calls.push_back(line + "]");
}

void sys_emit(ecs::event_writer<ping>& out, emitter_state& state) { out.emit({ state.next++ }); }
void sys_emit_late(ecs::event_writer<ping>& out, emitter_state& state) { out.emit({ 1000 + state.next++ }); }

void sys_read_before(const ecs::event_reader<ping>& in, seen_log<0>& log) { record(in, log); }
void sys_read_game_after(const ecs::event_reader<ping>& in, seen_log<1>& log) { record(in, log); }
void sys_read_post(const ecs::event_reader<ping>& in, seen_log<2>& log) { record(in, log); }
void sys_read_in_edit(const ecs::event_reader<ping>& in, seen_log<3>& log) { record(in, log); }

struct log_capture
{
	log_capture()
	{
		sink = engine::add_log_sink([this](std::string_view category, engine::log_level level, std::string_view message) {
			if (level >= engine::log_level::warn) lines.push_back(std::format("{}: {}", category, message));
		});
	}

	size_t count(std::string_view part) const
	{
		size_t n = 0;
		for (const auto& l : lines) {
			if (l.find(part) != std::string::npos) ++n;
		}
		return n;
	}

	std::vector<std::string> lines;
	engine::log_sink_handle sink; // last: removed first
};

struct bus_fixture : test_util::scene_env
{
	bus_fixture()
		: mgr(*rs, *assembler, sf)
	{
		sf.add_game_phase("test::after_update", ecs::phase::update, ecs::phase_side::after);
		sf.register_system<&sys_emit>("emit", { .phase = ecs::phase::update });
		sf.register_system<&sys_emit_late>("emit_late", { .phase = ecs::phase::late_update });
		sf.register_system<&sys_read_before>("read_before", { .phase = ecs::phase::pre_update });
		sf.register_system<&sys_read_game_after>("read_game_after", { .phase = "test::after_update" });
		sf.register_system<&sys_read_post>("read_post", { .phase = ecs::phase::post_update });
		sf.register_system<&sys_read_in_edit>("read_in_edit", { .phase = ecs::phase::pre_update, .run_in_edit = true });
		mgr.events().add<ping>();
	}

	scn::world_context& world(std::string_view set_name, std::vector<std::string> systems, std::string_view name = "w")
	{
		auto& set = mgr.find_set(set_name) ? *mgr.find_set(set_name) : mgr.create_set(set_name);
		auto& context = set.create_context(name, static_cast<uint32_t>(set.size()));
		set.add_systems(context, systems, sf);
		return context;
	}

	template <int N>
	static const std::vector<std::string>& calls(scn::world_context& w)
	{
		return w.state().ctx().get<seen_log<N>>().calls;
	}

	void frames(std::string_view set, int n)
	{
		for (int i = 0; i < n; ++i) mgr.get_set(set).update(one_frame);
	}

	ecs::system_factory sf;
	scn::context_manager mgr;
};

using lines = std::vector<std::string>;

} // namespace

BOOST_FIXTURE_TEST_SUITE(ScnWorldBusTests, bus_fixture)

BOOST_AUTO_TEST_CASE(EveryWorldGetsABusBoundToTheEventTypesOfItsSet)
{
	auto& w = world("s", {});
	BOOST_TEST(w.state().ctx().contains<ecs::world_bus>());
	BOOST_TEST(w.state().ctx().get<ecs::event_writer<ping>>().bound());
	BOOST_TEST(w.state().ctx().get<ecs::world_bus>().world() == "w");
}

BOOST_AUTO_TEST_CASE(Play_SystemsAfterThePointSeeThisFramesEvents_SystemsBeforeItTheNextFrame)
{
	auto& w = world("s", { "emit", "read_before", "read_game_after", "read_post" });
	frames("s", 3);

	BOOST_TEST((calls<0>(w) == lines{ "d0:[]", "d1:[0]", "d2:[1]" }));
	BOOST_TEST((calls<1>(w) == lines{ "d1:[0]", "d2:[1]", "d3:[2]" }));
	BOOST_TEST((calls<2>(w) == lines{ "d1:[0]", "d2:[1]", "d3:[2]" }));
}

BOOST_AUTO_TEST_CASE(Pause_KeepsTheWindowAndQueues_StepDeliversOnce)
{
	auto& w = world("s", { "emit", "read_post", "read_in_edit" });
	frames("s", 1);
	BOOST_TEST((calls<2>(w) == lines{ "d1:[0]" }));

	mgr.set_mode("s", ecs::run_mode::pause);
	mgr.emit("s", "w", ping{ 100 });
	frames("s", 2);
	// Only run_in_edit systems run; they see the same window, and the delivery number tells them so.
	BOOST_TEST((calls<3>(w) == lines{ "d0:[]", "d1:[0]", "d1:[0]" }));
	BOOST_TEST(calls<2>(w).size() == 1u);

	mgr.step("s", 1);
	frames("s", 1);
	BOOST_TEST((calls<2>(w) == lines{ "d1:[0]", "d2:[100,1]" }));

	frames("s", 1); // back in Pause
	BOOST_TEST((calls<3>(w).back() == "d2:[100,1]"));
	BOOST_TEST(calls<2>(w).size() == 2u);
}

BOOST_AUTO_TEST_CASE(Edit_DropsEventsAndLeavesTheWindowEmpty)
{
	auto& w = world("s", { "emit", "read_post", "read_in_edit" });
	frames("s", 1);

	mgr.set_mode("s", ecs::run_mode::edit);
	mgr.emit("s", "w", ping{ 5 });
	frames("s", 2);
	BOOST_TEST((calls<3>(w) == lines{ "d0:[]", "d0:[]", "d0:[]" }));
	BOOST_TEST(w.state().ctx().get<ecs::world_bus>().queued() == 0u);

	mgr.set_mode("s", ecs::run_mode::play);
	frames("s", 1);
	// The emitter ran once in Play before (0) and now (1); the post made in Edit is gone.
	BOOST_TEST((calls<2>(w).back() == "d2:[1]"));
}

BOOST_AUTO_TEST_CASE(AWorldCreatedInEditDropsFromTheStart)
{
	mgr.create_set("s").set_mode(ecs::run_mode::edit);
	auto& w = world("s", { "read_in_edit" });
	BOOST_TEST((w.state().ctx().get<ecs::world_bus>().mode() == ecs::bus_mode::discard));
	mgr.emit("s", "w", ping{ 1 });
	BOOST_TEST(w.state().ctx().get<ecs::world_bus>().queued() == 0u);
}

BOOST_AUTO_TEST_CASE(AnExternalEmitComesFirstWithAnEmptySender)
{
	auto& w = world("s", { "emit" });
	std::vector<std::string> seen;
	auto sub = w.state().ctx().get<ecs::world_bus>().subscribe<ping>([&](const ecs::envelope<ping>& e) {
		seen.push_back(std::format("{}>{}#{}:{}", e.from.empty() ? "ext" : e.from, e.to.context, e.seq, e.payload.value));
	});
	mgr.emit("s", "w", ping{ 42 });
	frames("s", 1);
	BOOST_TEST((seen == lines{ "ext>w#0:42", "w>w#1:0" }));
}

BOOST_AUTO_TEST_CASE(EachWorldHasItsOwnBus)
{
	auto& a = world("s", { "emit", "read_post" }, "a");
	auto& b = world("s", { "read_post" }, "b");
	mgr.emit("s", "b", ping{ 7 });
	frames("s", 1);
	BOOST_TEST((calls<2>(a) == lines{ "d1:[0]" }));
	BOOST_TEST((calls<2>(b) == lines{ "d1:[7]" }));
}

BOOST_AUTO_TEST_CASE(ALateSenderWaitsForTheNextDeliveryAndIsWarnedOncePerType)
{
	log_capture logs;
	auto& w = world("s", { "emit_late", "read_post" });
	frames("s", 3);
	BOOST_TEST((calls<2>(w) == lines{ "d1:[]", "d2:[1000]", "d3:[1001]" }));
#if SNK_ASSERTS_ENABLED
	BOOST_TEST(logs.count("emitted after the delivery point") == 1u);
#else
	BOOST_TEST(logs.count("emitted after the delivery point") == 0u);
#endif
}

BOOST_AUTO_TEST_CASE(AnEmitInsideTheUpdatePhaseIsNotLate)
{
	log_capture logs;
	world("s", { "emit", "read_post" });
	frames("s", 3);
	BOOST_TEST(logs.count("emitted after the delivery point") == 0u);
}

BOOST_AUTO_TEST_CASE(AnEventTypeRegisteredLaterIsBoundAtTheNextUpdate)
{
	auto& w = world("s", {});
	BOOST_TEST(!w.state().ctx().contains<ecs::event_writer<registered_later>>());
	mgr.events().add<registered_later>();
	frames("s", 1);
	BOOST_TEST(w.state().ctx().get<ecs::event_writer<registered_later>>().bound());
}

BOOST_AUTO_TEST_CASE(ALayersEventTypesReachOnlyItsWorlds_AndGoWithTheLayer)
{
	auto& layer = mgr.create_layer();
	BOOST_TEST(layer.events().contains<ping>()); // the base's, registered before the layer
	BOOST_TEST(layer.events().add<layer_event>());
	BOOST_TEST(!mgr.events().contains<layer_event>());

	auto& base_world = world("base", {});
	auto& layer_set = mgr.create_set("game", &layer);
	auto& layer_world = layer_set.create_context("w", 0);
	BOOST_TEST(layer_world.state().ctx().get<ecs::event_writer<layer_event>>().bound());
	BOOST_TEST(!base_world.state().ctx().contains<ecs::event_writer<layer_event>>());

	mgr.emit("game", "w", layer_event{ 3 });
	layer_set.update(one_frame);
	BOOST_TEST(layer_world.state().ctx().get<ecs::event_reader<layer_event>>().events().size() == 1u);

	// A module's unload: its worlds are destroyed with the layer, before the layer's registry and its types.
	mgr.destroy_layer(layer);
	BOOST_TEST(!mgr.find_set("game"));
	BOOST_TEST(mgr.find_set("base"));
	frames("base", 1);
	BOOST_TEST(!base_world.state().ctx().contains<ecs::event_writer<layer_event>>());
}

BOOST_AUTO_TEST_CASE(TwoRunsDeliverTheSameEvents)
{
	auto run = [this](std::string_view set) {
		auto& w = world(set, { "emit", "emit_late", "read_before", "read_game_after", "read_post" });
		for (int frame = 0; frame < 5; ++frame) {
			if (frame % 2 == 0) mgr.emit(set, "w", ping{ 500 + frame });
			mgr.get_set(set).update(one_frame);
		}
		lines all = calls<0>(w);
		all.insert(all.end(), calls<1>(w).begin(), calls<1>(w).end());
		all.insert(all.end(), calls<2>(w).begin(), calls<2>(w).end());
		return all;
	};
	const auto first = run("one");
	BOOST_TEST(!first.empty());
	BOOST_TEST((first == run("two")));
}

BOOST_AUTO_TEST_SUITE_END()
