#include <boost/test/unit_test.hpp>
#include <snk/ecs/ecs_bus.hpp>
#include <entt/entt.hpp>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

// The journal of the world bus (#605, WORLD_BUS.md 3.7): what world.events reads.

namespace {

struct gem
{
	int value = 0;
};

struct note
{
	int value = 0;
};

struct legacy_event
{
	int value = 0;
};

std::string write_gem(const gem& g)
{
	if (g.value < 0) throw std::runtime_error("a negative gem");
	if (g.value == 1000) return std::string(5000, 'x');
	return std::format(R"json({{"value":{}}})json", g.value);
}

// A type of a game module built before the journal: its bus_type has no bus_journal_source.
template <class T>
struct legacy_bus_type final : ecs::detail::bus_type
{
	entt::id_type hash() const override { return entt::type_hash<T>::value(); }
	std::string_view name() const override { return entt::type_name<T>::value(); }
	std::string_view schema() const override { return {}; }
	std::unique_ptr<ecs::detail::bus_slot> make_slot(const ecs::detail::bus_state* state) const override
	{
		return std::make_unique<ecs::detail::bus_slot_impl<T>>(state);
	}
	void bind(entt::registry&, ecs::detail::bus_slot&) const override {}
};

struct journal_fixture
{
	journal_fixture()
	{
		types.add<gem>({}, &write_gem);
		types.add<note>();
		types.add_type(std::make_unique<legacy_bus_type<legacy_event>>());
		bus().bind(types, reg, "World0");
	}

	ecs::world_bus& bus() { return reg.ctx().emplace<ecs::world_bus>(); }

	ecs::bus_registry types;
	entt::registry reg;
};

}

BOOST_FIXTURE_TEST_SUITE(ecs_bus_journal_tests, journal_fixture)

BOOST_AUTO_TEST_CASE(the_journal_is_off_until_it_is_enabled)
{
	BOOST_TEST((bus().journal() == nullptr));
	bus().post(gem{ 1 });
	bus().deliver();
	BOOST_TEST((bus().journal() == nullptr));

	bus().enable_journal(4);
	BOOST_REQUIRE((bus().journal() != nullptr));
	BOOST_TEST(bus().journal()->entries().empty());
	bus().disable_journal();
	BOOST_TEST((bus().journal() == nullptr));
}

BOOST_AUTO_TEST_CASE(the_journal_keeps_the_last_events_oldest_first_with_their_payload)
{
	bus().enable_journal(3);
	for (int i = 1; i <= 5; ++i) bus().post(gem{ i });
	bus().deliver();
	const auto& entries = bus().journal()->entries();
	BOOST_REQUIRE(entries.size() == 3u);
	BOOST_TEST(entries[0].seq == 2u);
	BOOST_TEST(entries[2].seq == 4u);
	BOOST_TEST(entries[2].payload == R"json({"value":5})json");
	BOOST_TEST(entries[2].delivery == 1u);
	BOOST_REQUIRE(entries[2].from.has_value());
	BOOST_TEST(entries[2].from->empty()); // an external post
	BOOST_TEST(entries[2].type.find("gem") != std::string::npos);

	bus().post(gem{ 6 });
	bus().deliver();
	BOOST_TEST(bus().journal()->entries().back().delivery == 2u);
	BOOST_TEST(bus().journal()->entries().size() == 3u);
}

BOOST_AUTO_TEST_CASE(a_type_without_a_writer_gives_only_the_envelope)
{
	bus().enable_journal(8);
	bus().post(note{ 3 });
	bus().deliver();
	const auto& entries = bus().journal()->entries();
	BOOST_REQUIRE(entries.size() == 1u);
	BOOST_TEST(entries[0].payload.empty());
	BOOST_TEST(entries[0].from.has_value());
}

BOOST_AUTO_TEST_CASE(a_payload_over_4_kib_is_replaced_by_a_marker)
{
	bus().enable_journal(8);
	bus().post(gem{ 1000 });
	bus().deliver();
	BOOST_TEST(bus().journal()->entries().back().payload == R"json({"truncated":true,"bytes":5000})json");
}

BOOST_AUTO_TEST_CASE(a_writer_that_throws_costs_neither_the_frame_nor_the_other_events)
{
	bus().enable_journal(8);
	int heard = 0;
	auto heard_gems = bus().subscribe<gem>([&heard](const ecs::envelope<gem>&) { ++heard; });
	bus().post(gem{ -1 });
	bus().post(gem{ 2 });
	bus().deliver();
	const auto& entries = bus().journal()->entries();
	BOOST_REQUIRE(entries.size() == 2u);
	BOOST_TEST(entries[0].payload == R"json({"error":"writer threw"})json");
	BOOST_TEST(entries[1].payload == R"json({"value":2})json");
	BOOST_TEST(heard == 2); // the delivery went on
}

BOOST_AUTO_TEST_CASE(a_type_of_a_module_built_before_the_journal_gives_only_its_place)
{
	bus().enable_journal(8);
	bus().post(legacy_event{ 1 });
	bus().post(legacy_event{ 2 });
	bus().deliver();
	const auto& entries = bus().journal()->entries();
	BOOST_REQUIRE(entries.size() == 2u);
	BOOST_TEST(!entries[0].from.has_value());
	BOOST_TEST(entries[0].payload.empty());
	BOOST_TEST(entries[1].seq == 1u);
	BOOST_TEST(entries[1].type.find("legacy_event") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(clearing_the_bus_empties_the_journal)
{
	bus().enable_journal(8);
	bus().post(gem{ 1 });
	bus().deliver();
	BOOST_REQUIRE(!bus().journal()->entries().empty());
	bus().clear();
	BOOST_TEST(bus().journal()->entries().empty());
}

BOOST_AUTO_TEST_SUITE_END()
