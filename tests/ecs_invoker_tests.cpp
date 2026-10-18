#include <boost/test/unit_test.hpp>
#include "ecs_system.h"
#include <entt/entt.hpp>
#include <vector>

// Argument injection of ecs::invoker: registry, views, ctx resources and cross-world bind_res.
// Plus system_factory behavior that does not need a full level.

namespace {

struct hp { int value = 0; };
struct tick_counter { int ticks = 0; };
struct shared_state { int value = 0; };

int g_sum = 0;
int g_seen_world_value = -1;
entt::registry* g_seen_registry = nullptr;

void sys_registry(entt::registry& reg) { g_seen_registry = &reg; }

void sys_view(entt::view<entt::get_t<hp>> view)
{
	g_sum = 0;
	for (auto e : view) g_sum += view.get<hp>(e).value;
}

void sys_increment_resource(tick_counter& counter) { ++counter.ticks; }

void sys_read_other_world(const ecs::bind_res<1, shared_state>& other) { g_seen_world_value = other->value; }

void sys_write_other_world(ecs::bind_res<1, shared_state> other) { other->value += 5; }

struct stateful
{
	int calls = 0;
	int last_sum = 0;
	void run(tick_counter& counter, entt::view<entt::get_t<hp>> view)
	{
		++calls;
		last_sum = 0;
		for (auto e : view) last_sum += view.get<hp>(e).value;
		counter.ticks += 10;
	}
};

struct invoker_fixture
{
	invoker_fixture()
	{
		g_sum = 0;
		g_seen_world_value = -1;
		g_seen_registry = nullptr;
		ctx.current_registry = &world0;
		ctx.world_id = 0;
		ctx.getter = [this](size_t id) -> entt::registry& { return id == 0 ? world0 : world1; };
	}

	// runtime_context holds a reference member, so it is rebuilt on demand
	ecs::runtime_context make()
	{
		return ecs::runtime_context{ .get_world_registry = ctx.getter, .current_registry = *ctx.current_registry, .current_world_id = ctx.world_id };
	}

	struct holder
	{
		entt::registry* current_registry = nullptr;
		size_t world_id = 0;
		std::function<entt::registry&(size_t)> getter;
	} ctx;

	entt::registry world0;
	entt::registry world1;
};

}

BOOST_FIXTURE_TEST_SUITE(EcsInvokerTests, invoker_fixture)

BOOST_AUTO_TEST_CASE(RegistryArgumentIsTheCurrentWorldRegistry)
{
	ecs::invoker<&sys_registry>::invoke(make());
	BOOST_TEST((g_seen_registry == &world0));
}

BOOST_AUTO_TEST_CASE(ViewArgumentIteratesComponentsOfCurrentWorld)
{
	for (int v : { 1, 2, 3 }) world0.emplace<hp>(world0.create(), hp{ v });
	world1.emplace<hp>(world1.create(), hp{ 100 }); // other world must not leak in
	ecs::invoker<&sys_view>::invoke(make());
	BOOST_TEST(g_sum == 6);
}

BOOST_AUTO_TEST_CASE(PlainReferenceArgumentIsAContextSingletonCreatedOnDemand)
{
	BOOST_TEST(world0.ctx().find<tick_counter>() == nullptr);
	ecs::invoker<&sys_increment_resource>::invoke(make());
	ecs::invoker<&sys_increment_resource>::invoke(make());
	ecs::invoker<&sys_increment_resource>::invoke(make());
	BOOST_REQUIRE(world0.ctx().find<tick_counter>() != nullptr);
	BOOST_TEST(world0.ctx().get<tick_counter>().ticks == 3); // same object every call
	BOOST_TEST(world1.ctx().find<tick_counter>() == nullptr);
}

BOOST_AUTO_TEST_CASE(BindResReadsAndWritesAnotherWorldsContext)
{
	world1.ctx().emplace<shared_state>().value = 7;

	ecs::invoker<&sys_read_other_world>::invoke(make());
	BOOST_TEST(g_seen_world_value == 7);

	ecs::invoker<&sys_write_other_world>::invoke(make());
	BOOST_TEST(world1.ctx().get<shared_state>().value == 12);
}

BOOST_AUTO_TEST_CASE(MemberFunctionInvokerPassesInstanceAndInjectedArguments)
{
	world0.emplace<hp>(world0.create(), hp{ 4 });
	world0.emplace<hp>(world0.create(), hp{ 5 });
	stateful s;
	ecs::invoker<&stateful::run>::invoke(s, make());
	ecs::invoker<&stateful::run>::invoke(s, make());
	BOOST_TEST(s.calls == 2);
	BOOST_TEST(s.last_sum == 9);
	BOOST_TEST(world0.ctx().get<tick_counter>().ticks == 20);
}

BOOST_AUTO_TEST_CASE(FunctionTraitsReportArgumentCountAndTypes)
{
	using t = ecs::function_traits<decltype(&sys_increment_resource)>;
	static_assert(t::args_count == 1);
	static_assert(std::is_same_v<t::arg_type<0>, tick_counter&>);
	using m = ecs::function_traits<decltype(&stateful::run)>;
	static_assert(m::args_count == 2);
	BOOST_TEST(true);
}

// ---------------------------------------------------------------- system_factory

BOOST_AUTO_TEST_CASE(Factory_UnknownSystemIsNotInTheRegistry)
{
	ecs::system_factory factory;
	BOOST_TEST(factory.registry().find("nope") == nullptr);
}

struct stateful_system : ecs::system_interface
{
	explicit stateful_system(int initial) : counter(initial) {}
	void run(tick_counter& c) { ++counter; c.ticks = counter; }
	int counter;
};

BOOST_AUTO_TEST_CASE(Factory_StatefulSystemIsConstructedPerInstanceWithRegisteredArguments)
{
	ecs::system_factory factory;
	factory.register_system<&stateful_system::run, stateful_system>("st", {}, 100);

	const auto* entry = factory.registry().find("st");
	BOOST_REQUIRE(entry != nullptr);
	auto a = entry->make();
	auto b = entry->make();
	BOOST_REQUIRE(a.owner);
	BOOST_REQUIRE(b.owner);
	BOOST_TEST(a.owner != b.owner); // separate instance per world
	BOOST_TEST(static_cast<stateful_system&>(*a.owner).counter == 100);

	entt::registry reg;
	auto ctx = ecs::runtime_context{ .get_world_registry = [&reg](size_t) -> entt::registry& { return reg; }, .current_registry = reg };
	entry->run(a.self, ctx);
	BOOST_TEST(static_cast<stateful_system&>(*a.owner).counter == 101);
	BOOST_TEST(static_cast<stateful_system&>(*b.owner).counter == 100); // the other instance is untouched
	BOOST_TEST(reg.ctx().get<tick_counter>().ticks == 101);
}

BOOST_AUTO_TEST_CASE(Factory_SharedInstanceOverloadHandsOutTheSameObject)
{
	ecs::system_factory factory;
	auto inst = std::make_shared<stateful_system>(5);
	factory.register_system<&stateful_system::run>("shared", {}, inst);

	const auto* entry = factory.registry().find("shared");
	BOOST_REQUIRE(entry != nullptr);
	BOOST_TEST(entry->make().owner == inst);
}

BOOST_AUTO_TEST_CASE(Factory_StatelessFunctionHasNoInstanceAndRunsThroughTheInvoker)
{
	ecs::system_factory factory;
	factory.register_system<&sys_increment_resource>("tick");

	const auto* entry = factory.registry().find("tick");
	BOOST_REQUIRE(entry != nullptr);
	auto inst = entry->make();
	BOOST_TEST(inst.owner == nullptr);

	entt::registry reg;
	auto ctx = ecs::runtime_context{ .get_world_registry = [&reg](size_t) -> entt::registry& { return reg; }, .current_registry = reg };
	entry->run(inst.self, ctx);
	entry->run(inst.self, ctx);
	BOOST_TEST(reg.ctx().get<tick_counter>().ticks == 2);
}

BOOST_AUTO_TEST_SUITE_END()
