#include <boost/test/unit_test.hpp>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_signal.hpp>
#include <entt/entt.hpp>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// The world bus core (#603 PR 1, WORLD_BUS.md 3): ordering, seq, window, limit, modes, subscriptions. No scene here:
// a test plays the world by calling the systems' writers and world_bus::deliver() itself.

namespace {

struct coin
{
	int value = 0;
};

struct hit
{
	int damage = 0;
};

struct unregistered_event
{
	int x = 0;
};

using record = std::tuple<std::string, std::string, uint32_t, int>; // from, to, seq, payload

template <class T, class F>
std::vector<record> records_of(std::span<const ecs::envelope<T>> events, F payload_of)
{
	std::vector<record> out;
	for (const auto& e : events) out.emplace_back(e.from, e.to.context, e.seq, payload_of(e.payload));
	return out;
}

std::vector<record> coins(const entt::registry& reg)
{
	return records_of(reg.ctx().get<ecs::event_reader<coin>>().events(), [](const coin& c) { return c.value; });
}

struct bus_fixture
{
	bus_fixture()
	{
		types.add<coin>();
		types.add<hit>();
		bus().bind(types, reg, "World0");
	}

	ecs::world_bus& bus() { return reg.ctx().emplace<ecs::world_bus>(); }
	ecs::event_writer<coin>& coin_out() { return reg.ctx().get<ecs::event_writer<coin>>(); }
	ecs::event_writer<hit>& hit_out() { return reg.ctx().get<ecs::event_writer<hit>>(); }

	ecs::bus_registry types;
	entt::registry reg;
};

} // namespace

BOOST_AUTO_TEST_SUITE(ecs_signal_tests)

BOOST_AUTO_TEST_CASE(slots_run_in_subscription_order)
{
	ecs::signal<int> sig;
	std::vector<std::string> log;
	auto a = sig.subscribe([&](int v) { log.push_back("a" + std::to_string(v)); });
	auto b = sig.subscribe([&](int v) { log.push_back("b" + std::to_string(v)); });
	sig.emit(1);
	BOOST_TEST(log == (std::vector<std::string>{ "a1", "b1" }));
	BOOST_TEST(sig.subscriber_count() == 2u);
}

BOOST_AUTO_TEST_CASE(a_slot_removed_during_an_emit_is_not_called_and_an_added_one_runs_next_time)
{
	ecs::signal<> sig;
	std::vector<std::string> log;
	ecs::connection second;
	ecs::connection late;
	auto first = sig.subscribe([&] {
		log.push_back("first");
		second.disconnect();
		if (!late.connected()) late = sig.subscribe([&] { log.push_back("late"); });
	});
	second = sig.subscribe([&] { log.push_back("second"); });

	sig.emit();
	BOOST_TEST(log == (std::vector<std::string>{ "first" }));
	log.clear();
	sig.emit();
	BOOST_TEST(log == (std::vector<std::string>{ "first", "late" }));
}

BOOST_AUTO_TEST_CASE(a_slot_can_disconnect_itself)
{
	ecs::signal<> sig;
	int calls = 0;
	ecs::connection self;
	self = sig.subscribe([&] {
		++calls;
		self.disconnect();
	});
	sig.emit();
	sig.emit();
	BOOST_TEST(calls == 1);
	BOOST_TEST(sig.subscriber_count() == 0u);
}

BOOST_AUTO_TEST_CASE(a_connection_outlives_its_signal)
{
	ecs::connection c;
	{
		ecs::signal<> sig;
		c = sig.subscribe([] {});
	}
	BOOST_TEST(c.connected());
	c.disconnect();
	BOOST_TEST(!c.connected());
}

BOOST_AUTO_TEST_CASE(a_moved_connection_disconnects_once)
{
	ecs::signal<> sig;
	int calls = 0;
	std::optional<ecs::connection> kept;
	{
		auto c = sig.subscribe([&] { ++calls; });
		kept.emplace(std::move(c));
	}
	sig.emit();
	BOOST_TEST(calls == 1);
	kept.reset();
	sig.emit();
	BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(ecs_bus_tests, bus_fixture)

BOOST_AUTO_TEST_CASE(binding_gives_the_registry_a_writer_and_a_reader_per_type)
{
	BOOST_TEST(coin_out().bound());
	BOOST_TEST(reg.ctx().get<ecs::event_reader<hit>>().bound());
	BOOST_TEST(!reg.ctx().emplace<ecs::event_writer<unregistered_event>>().bound());
	BOOST_TEST(reg.ctx().get<ecs::event_reader<coin>>().delivery() == 0u);
	BOOST_TEST(reg.ctx().get<ecs::event_reader<coin>>().events().empty());
}

BOOST_AUTO_TEST_CASE(nothing_is_visible_before_the_delivery)
{
	coin_out().emit({ 1 });
	BOOST_TEST(coins(reg).empty());
	BOOST_TEST(bus().queued() == 1u);
	bus().deliver();
	BOOST_TEST((coins(reg) == std::vector<record>{ { "World0", "World0", 0u, 1 } }));
	BOOST_TEST(bus().queued() == 0u);
}

BOOST_AUTO_TEST_CASE(external_posts_go_first_with_an_empty_sender)
{
	coin_out().emit({ 1 });
	bus().post(coin{ 100 });
	coin_out().emit({ 2 });
	bus().deliver();
	BOOST_TEST((coins(reg) == std::vector<record>{
		{ "", "World0", 0u, 100 },
		{ "World0", "World0", 1u, 1 },
		{ "World0", "World0", 2u, 2 },
	}));
}

BOOST_AUTO_TEST_CASE(what_a_subscriber_sends_comes_in_the_next_delivery_after_posts_and_before_systems)
{
	auto sub = bus().subscribe<hit>([&](const ecs::envelope<hit>& e) { coin_out().emit({ e.payload.damage * 10 }); });
	hit_out().emit({ 1 });
	bus().deliver();
	BOOST_TEST(coins(reg).empty());

	coin_out().emit({ 2 });
	bus().post(coin{ 100 });
	bus().deliver();
	BOOST_TEST((coins(reg) == std::vector<record>{
		{ "", "World0", 0u, 100 },
		{ "World0", "World0", 1u, 10 },
		{ "World0", "World0", 2u, 2 },
	}));
}

BOOST_AUTO_TEST_CASE(subscribers_run_per_event_in_subscription_order_types_in_registration_order)
{
	std::vector<std::string> log;
	auto c1 = bus().subscribe<coin>([&](const ecs::envelope<coin>& e) { log.push_back("c1:" + std::to_string(e.payload.value)); });
	auto h = bus().subscribe<hit>([&](const ecs::envelope<hit>& e) { log.push_back("h:" + std::to_string(e.payload.damage)); });
	auto c2 = bus().subscribe<coin>([&](const ecs::envelope<coin>& e) { log.push_back("c2:" + std::to_string(e.payload.value)); });

	hit_out().emit({ 7 });
	coin_out().emit({ 1 });
	coin_out().emit({ 2 });
	bus().deliver();
	BOOST_TEST(log == (std::vector<std::string>{ "c1:1", "c2:1", "c1:2", "c2:2", "h:7" }));
}

BOOST_AUTO_TEST_CASE(a_subscriber_reads_the_new_windows_of_every_type)
{
	size_t hits_seen = 0;
	auto sub = bus().subscribe<coin>([&](const ecs::envelope<coin>&) {
		hits_seen = reg.ctx().get<ecs::event_reader<hit>>().events().size();
	});
	coin_out().emit({ 1 });
	hit_out().emit({ 1 });
	hit_out().emit({ 2 });
	bus().deliver();
	BOOST_TEST(hits_seen == 2u);
}

BOOST_AUTO_TEST_CASE(subscribing_and_unsubscribing_inside_a_delivery)
{
	std::vector<std::string> log;
	ecs::connection second;
	ecs::connection late;
	auto first = bus().subscribe<coin>([&](const ecs::envelope<coin>& e) {
		log.push_back("first:" + std::to_string(e.payload.value));
		second.disconnect();
		if (!late.connected()) {
			late = bus().subscribe<coin>([&](const ecs::envelope<coin>& l) { log.push_back("late:" + std::to_string(l.payload.value)); });
		}
	});
	second = bus().subscribe<coin>([&](const ecs::envelope<coin>& e) { log.push_back("second:" + std::to_string(e.payload.value)); });

	coin_out().emit({ 1 });
	bus().deliver();
	// "late" was added while the first event was being delivered: it runs from the next event on.
	BOOST_TEST(log == (std::vector<std::string>{ "first:1" }));

	log.clear();
	coin_out().emit({ 2 });
	bus().deliver();
	BOOST_TEST(log == (std::vector<std::string>{ "first:2", "late:2" }));
}

BOOST_AUTO_TEST_CASE(the_window_is_replaced_only_by_the_next_delivery)
{
	coin_out().emit({ 1 });
	bus().deliver();
	const auto& reader = reg.ctx().get<ecs::event_reader<coin>>();
	BOOST_TEST(reader.delivery() == 1u);

	// Pause: the queue grows, the window and its number stay.
	coin_out().emit({ 2 });
	BOOST_TEST((coins(reg) == std::vector<record>{ { "World0", "World0", 0u, 1 } }));
	BOOST_TEST(reader.delivery() == 1u);

	bus().deliver();
	BOOST_TEST((coins(reg) == std::vector<record>{ { "World0", "World0", 0u, 2 } }));
	BOOST_TEST(reader.delivery() == 2u);

	bus().deliver();
	BOOST_TEST(coins(reg).empty());
	BOOST_TEST(reader.delivery() == 3u);
	BOOST_TEST(bus().deliveries() == 3u);
}

BOOST_AUTO_TEST_CASE(a_delivery_takes_at_most_the_limit_per_type)
{
	BOOST_TEST(bus().event_limit() == ecs::default_event_limit);
	bus().set_event_limit(3);
	for (int i = 0; i < 5; ++i) coin_out().emit({ i });
	bus().post(coin{ 100 });
	hit_out().emit({ 1 });
	BOOST_TEST(bus().queued() == 4u);

	bus().deliver();
	BOOST_TEST((coins(reg) == std::vector<record>{
		{ "World0", "World0", 0u, 0 },
		{ "World0", "World0", 1u, 1 },
		{ "World0", "World0", 2u, 2 },
	}));
	BOOST_TEST(reg.ctx().get<ecs::event_reader<hit>>().events().size() == 1u);

	for (int i = 0; i < 3; ++i) coin_out().emit({ i });
	bus().deliver();
	BOOST_TEST(coins(reg).size() == 3u);
}

BOOST_AUTO_TEST_CASE(discard_mode_drops_everything_and_does_not_deliver)
{
	coin_out().emit({ 1 });
	bus().deliver();
	coin_out().emit({ 2 });

	bus().set_mode(ecs::bus_mode::discard);
	BOOST_TEST(coins(reg).empty());
	BOOST_TEST(bus().queued() == 0u);

	coin_out().emit({ 3 });
	bus().post(coin{ 4 });
	BOOST_TEST(bus().queued() == 0u);
	bus().deliver();
	BOOST_TEST(bus().deliveries() == 1u);
	BOOST_TEST(coins(reg).empty());

	bus().set_mode(ecs::bus_mode::queue);
	coin_out().emit({ 5 });
	bus().deliver();
	BOOST_TEST((coins(reg) == std::vector<record>{ { "World0", "World0", 0u, 5 } }));
}

BOOST_AUTO_TEST_CASE(clear_drops_the_queue_and_the_window_and_counts_them)
{
	coin_out().emit({ 1 });
	coin_out().emit({ 2 });
	bus().deliver();
	coin_out().emit({ 3 });
	hit_out().emit({ 1 });
	BOOST_TEST(bus().clear() == 4u);
	BOOST_TEST(coins(reg).empty());
	BOOST_TEST(bus().queued() == 0u);
	bus().deliver();
	BOOST_TEST(coins(reg).empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ecs_bus_registry_tests)

BOOST_AUTO_TEST_CASE(a_type_is_registered_once_with_its_schema)
{
	ecs::bus_registry types;
	BOOST_TEST(types.add<coin>(R"({"type":"object"})"));
	BOOST_TEST(!types.add<coin>());
	BOOST_TEST(types.size() == 1u);
	BOOST_TEST((types.type(0).schema() == R"({"type":"object"})"));
	BOOST_TEST((types.type(0).name() == entt::type_name<coin>::value()));
}

BOOST_AUTO_TEST_CASE(a_layer_sees_the_parent_types_first_and_cannot_take_them)
{
	ecs::bus_registry base;
	base.add<coin>();
	ecs::bus_registry layer(&base);
	BOOST_TEST(!layer.add<coin>());
	BOOST_TEST(layer.add<hit>());
	BOOST_TEST(*layer.index_of<coin>() == 0u);
	BOOST_TEST(*layer.index_of<hit>() == 1u);
	BOOST_TEST(!base.contains<hit>());

	// Types the parent gets after the layer was made are not seen by it.
	base.add<unregistered_event>();
	BOOST_TEST(!layer.contains<unregistered_event>());
}

BOOST_AUTO_TEST_CASE(binding_again_binds_only_the_new_types)
{
	ecs::bus_registry types;
	types.add<coin>();
	entt::registry reg;
	auto& bus = reg.ctx().emplace<ecs::world_bus>();
	bus.bind(types, reg, "World0");
	reg.ctx().get<ecs::event_writer<coin>>().emit({ 1 });

	types.add<hit>();
	bus.bind(types, reg, "World0");
	BOOST_TEST(reg.ctx().get<ecs::event_writer<hit>>().bound());
	BOOST_TEST(bus.queued() == 1u);
	bus.deliver();
	BOOST_TEST(coins(reg).size() == 1u);
}

BOOST_AUTO_TEST_CASE(two_runs_deliver_the_same_sequence)
{
	auto run = [] {
		ecs::bus_registry types;
		types.add<coin>();
		types.add<hit>();
		entt::registry reg;
		auto& bus = reg.ctx().emplace<ecs::world_bus>();
		bus.bind(types, reg, "World0");
		auto& coin_out = reg.ctx().get<ecs::event_writer<coin>>();
		auto& hit_out = reg.ctx().get<ecs::event_writer<hit>>();
		auto sub = bus.subscribe<hit>([&](const ecs::envelope<hit>& e) { coin_out.emit({ -e.payload.damage }); });

		std::vector<record> seen;
		for (int frame = 0; frame < 4; ++frame) {
			if (frame % 2 == 0) bus.post(coin{ 1000 + frame });
			for (int i = 0; i <= frame; ++i) {
				coin_out.emit({ frame * 10 + i });
				hit_out.emit({ i });
			}
			bus.deliver();
			const auto window = coins(reg);
			seen.insert(seen.end(), window.begin(), window.end());
		}
		return seen;
	};
	const auto first = run();
	BOOST_TEST(!first.empty());
	BOOST_TEST((first == run()));
}

BOOST_AUTO_TEST_SUITE_END()
