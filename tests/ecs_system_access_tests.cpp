#include <boost/test/unit_test.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/ecs/ecs_system_plan.hpp>
#include <string>
#include <vector>

// Debug validation of unordered conflicting access, deduced from the system argument types.
// The access never decides the order, it only produces warnings.

namespace {

struct pos { float x = 0; };
struct vel { float x = 0; };
struct counter { int value = 0; };
struct flag { bool on = false; };

void write_pos(entt::view<entt::get_t<pos>>) {}
void write_pos_too(entt::view<entt::get_t<pos>>) {}
void write_pos_3(entt::view<entt::get_t<pos>>) {}
void read_pos(entt::view<entt::get_t<const pos>>) {}
void read_pos_too(entt::view<entt::get_t<const pos>>) {}
void write_vel(entt::view<entt::get_t<vel>>) {}
void write_counter(counter&) {}
void read_counter(const counter&) {}
void read_counter_too(const counter&) {}
void touch_registry(entt::registry&) {}
void bound_write(ecs::bind_res<1, counter>) {}
void bound_write_too(ecs::bind_res<1, counter>) {}
void bound_read(const ecs::bind_res<1, const counter>&) {}
void mixed(entt::view<entt::get_t<pos, const vel>>, counter&, const flag&, flag) {}

struct fixture {
	template <auto Fn>
	void add(const char* name, ecs::system_desc desc = {})
	{
		sf.register_system<Fn>(name, std::move(desc));
	}

	ecs::system_plan build(std::vector<std::string> names, bool validate = true)
	{
		return ecs::build_system_plan(sf.registry(), names, { .validate_access = validate });
	}

	static size_t conflicts(const ecs::system_plan& plan)
	{
		size_t n = 0;
		for (const auto& d : plan.diagnostics) n += d.kind == ecs::diagnostic_kind::access_conflict;
		return n;
	}

	ecs::system_factory sf;
};

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

}

BOOST_FIXTURE_TEST_SUITE(EcsSystemAccessTests, fixture)

BOOST_AUTO_TEST_CASE(Access_IsDeducedFromArgumentTypes)
{
	add<&mixed>("mixed");
	const auto& access = sf.registry().find("mixed")->access;

	BOOST_REQUIRE(access.size() == 5u);
	BOOST_TEST(access[0].type == entt::type_hash<pos>::value());
	BOOST_TEST(access[0].write);
	BOOST_TEST(access[1].type == entt::type_hash<vel>::value());
	BOOST_TEST(!access[1].write); // const component
	BOOST_TEST(access[2].type == entt::type_hash<counter>::value());
	BOOST_TEST(access[2].write);  // counter&
	BOOST_TEST(access[3].type == entt::type_hash<flag>::value());
	BOOST_TEST(!access[3].write); // const flag&
	BOOST_TEST(access[4].type == entt::type_hash<flag>::value());
	BOOST_TEST(!access[4].write); // by value is a read
}

BOOST_AUTO_TEST_CASE(TwoUnorderedWriters_Warn)
{
	add<&write_pos>("A");
	add<&write_pos_too>("B");

	auto plan = build({ "A", "B" });
	BOOST_REQUIRE(conflicts(plan) == 1u);
	const auto& msg = plan.diagnostics.front().message;
	BOOST_TEST(contains(msg, "'A'"));
	BOOST_TEST(contains(msg, "'B'"));
	BOOST_TEST(contains(msg, "both write"));
	BOOST_TEST(contains(msg, "pos"));
	BOOST_TEST(contains(msg, "'update'"));
	BOOST_TEST(contains(msg, "no order"));
	BOOST_TEST(!plan.has_errors()); // a warning, the plan still runs
	BOOST_TEST(plan.system_count() == 2u);
}

BOOST_AUTO_TEST_CASE(OrderedWriters_DoNotWarn)
{
	add<&write_pos>("A");
	add<&write_pos_too>("B", { .after = { "A" } });
	BOOST_TEST(conflicts(build({ "A", "B" })) == 0u);
}

BOOST_AUTO_TEST_CASE(ChainOfConstraints_CountsAsOrdered)
{
	add<&write_pos>("A");
	add<&write_vel>("M", { .after = { "A" } });
	add<&write_pos_too>("C", { .after = { "M" } });
	BOOST_TEST(conflicts(build({ "A", "M", "C" })) == 0u);
}

BOOST_AUTO_TEST_CASE(ReaderAndWriter_Warn)
{
	add<&write_pos>("A");
	add<&read_pos>("B");

	auto plan = build({ "A", "B" });
	BOOST_REQUIRE(conflicts(plan) == 1u);
	BOOST_TEST(contains(plan.diagnostics.front().message, "read and write"));
}

BOOST_AUTO_TEST_CASE(TwoReaders_DoNotWarn)
{
	add<&read_pos>("A");
	add<&read_pos_too>("B");
	add<&read_counter>("C");
	add<&read_counter_too>("D");
	BOOST_TEST(conflicts(build({ "A", "B", "C", "D" })) == 0u);
}

BOOST_AUTO_TEST_CASE(DifferentPhases_DoNotWarn)
{
	add<&write_pos>("A", { .phase = ecs::phase::update });
	add<&write_pos_too>("B", { .phase = ecs::phase::late_update });
	BOOST_TEST(conflicts(build({ "A", "B" })) == 0u);
}

BOOST_AUTO_TEST_CASE(DisjointData_DoesNotWarn)
{
	add<&write_pos>("A");
	add<&write_vel>("B");
	BOOST_TEST(conflicts(build({ "A", "B" })) == 0u);
}

BOOST_AUTO_TEST_CASE(ResourceWriterAndReader_Warn)
{
	add<&write_counter>("A");
	add<&read_counter>("B");
	BOOST_TEST(conflicts(build({ "A", "B" })) == 1u);
}

BOOST_AUTO_TEST_CASE(RegistryArgument_IsNotTracked)
{
	add<&touch_registry>("A");
	add<&write_pos>("B");
	BOOST_TEST(conflicts(build({ "A", "B" })) == 0u);
}

BOOST_AUTO_TEST_CASE(BoundResource_ConflictsOnlyWithinTheSameTargetWorld)
{
	add<&write_counter>("own");     // counter of the system's own world
	add<&bound_write>("w1_a");      // counter of world 1
	add<&bound_write_too>("w1_b");
	add<&bound_read>("w1_r");

	BOOST_TEST(conflicts(build({ "own", "w1_a" })) == 0u); // different worlds

	auto plan = build({ "w1_a", "w1_b" });
	BOOST_REQUIRE(conflicts(plan) == 1u);
	BOOST_TEST(contains(plan.diagnostics.front().message, "world 1"));

	BOOST_TEST(conflicts(build({ "w1_a", "w1_r" })) == 1u);
}

BOOST_AUTO_TEST_CASE(Validation_CanBeSwitchedOff)
{
	add<&write_pos>("A");
	add<&write_pos_too>("B");
	BOOST_TEST(conflicts(build({ "A", "B" }, false)) == 0u);
}

BOOST_AUTO_TEST_CASE(ManyUnorderedWriters_ReportEveryPair)
{
	add<&write_pos>("A");
	add<&write_pos_too>("B");
	add<&write_pos_3>("C");
	BOOST_TEST(conflicts(build({ "A", "B", "C" })) == 3u);
}

BOOST_AUTO_TEST_SUITE_END()
