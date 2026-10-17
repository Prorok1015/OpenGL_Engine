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

BOOST_AUTO_TEST_CASE(DependencyExtractorSaltsTypesPerWorld)
{
	using deps0 = ecs::args_to_dependencies<std::tuple<tick_counter&>, 0>::type;
	using deps1 = ecs::args_to_dependencies<std::tuple<tick_counter&>, 1>::type;
	static_assert(!std::is_same_v<deps0, deps1>); // same resource in different worlds is not a conflict
	static_assert(std::is_same_v<deps0, ecs::type_list<ecs::salted_type<tick_counter, 0>>>);

	// bind_res targets the world named by its template id, not the system's own world
	using bound = ecs::args_to_dependencies<std::tuple<const ecs::bind_res<1, shared_state>&>, 0>::type;
	static_assert(std::is_same_v<bound, ecs::type_list<ecs::salted_type<shared_state, 1>>>);
	using bound_const = ecs::args_to_dependencies<std::tuple<ecs::bind_res<1, const shared_state>>, 0>::type;
	static_assert(std::is_same_v<bound_const, ecs::type_list<const ecs::salted_type<shared_state, 1>>>); // read-only access stays const
	BOOST_TEST(true);
}

// ---------------------------------------------------------------- system_factory

BOOST_AUTO_TEST_CASE(Factory_UnknownSystemYieldsNull)
{
	ecs::system_factory factory;
	entt::registry reg;
	reg.ctx().emplace<ecs::world_salt>(ecs::world_salt{ 0 });
	entt::organizer org;
	BOOST_TEST(factory.create_system("nope", reg, org) == nullptr);
	BOOST_TEST(org.graph().empty());
}

BOOST_AUTO_TEST_CASE(Factory_StatelessFunctionRegistersOneVertexAndReturnsNoInstance)
{
	ecs::system_factory factory;
	factory.register_automatic_system<&sys_increment_resource>("tick");
	entt::registry reg;
	reg.ctx().emplace<ecs::world_salt>(ecs::world_salt{ 2 });
	entt::organizer org;

	BOOST_TEST(factory.create_system("tick", reg, org) == nullptr); // stateless -> no instance
	BOOST_REQUIRE(org.graph().size() == 1u);
	BOOST_TEST(std::string(org.graph()[0].name()) == "tick");
}

struct stateful_system : ecs::system_interface
{
	explicit stateful_system(int initial) : counter(initial) {}
	void run(tick_counter& c) { ++counter; c.ticks = counter; }
	int counter;
};

BOOST_AUTO_TEST_CASE(Factory_StatefulSystemIsConstructedPerWorldWithRegisteredArguments)
{
	ecs::system_factory factory;
	factory.register_automatic_system<&stateful_system::run, stateful_system>("st", 100);

	entt::registry reg_a, reg_b;
	reg_a.ctx().emplace<ecs::world_salt>(ecs::world_salt{ 0 });
	reg_b.ctx().emplace<ecs::world_salt>(ecs::world_salt{ 1 });
	entt::organizer org;

	auto a = factory.create_system("st", reg_a, org);
	auto b = factory.create_system("st", reg_b, org);
	BOOST_REQUIRE(a);
	BOOST_REQUIRE(b);
	BOOST_TEST(a != b); // separate instance per world
	BOOST_TEST(static_cast<stateful_system&>(*a).counter == 100);
	BOOST_TEST(org.graph().size() == 2u);
}

BOOST_AUTO_TEST_CASE(Factory_SharedInstanceOverloadReturnsTheSameObject)
{
	ecs::system_factory factory;
	auto inst = std::make_shared<stateful_system>(5);
	factory.register_automatic_system<&stateful_system::run>("shared", inst);

	entt::registry reg;
	reg.ctx().emplace<ecs::world_salt>(ecs::world_salt{ 0 });
	entt::organizer org;
	BOOST_TEST(factory.create_system("shared", reg, org) == inst);
}

BOOST_AUTO_TEST_CASE(Factory_SystemsSharingAWritableResourceGetOrderedEdges)
{
	ecs::system_factory factory;
	factory.register_automatic_system<&sys_increment_resource>("w1");
	factory.register_automatic_system<&sys_increment_resource>("w2");
	entt::registry reg;
	reg.ctx().emplace<ecs::world_salt>(ecs::world_salt{ 0 });
	entt::organizer org;
	factory.create_system("w1", reg, org);
	factory.create_system("w2", reg, org);

	auto graph = org.graph();
	BOOST_REQUIRE(graph.size() == 2u);
	// both write tick_counter in world 0 -> the second must run after the first
	BOOST_TEST(graph[0].out_edges().size() + graph[1].out_edges().size() == 1u);
}

BOOST_AUTO_TEST_CASE(Factory_SameResourceInDifferentWorldsIsADistinctDependency)
{
	ecs::system_factory factory;
	factory.register_automatic_system<&sys_increment_resource>("w");
	entt::registry r0, r1;
	r0.ctx().emplace<ecs::world_salt>(ecs::world_salt{ 0 });
	r1.ctx().emplace<ecs::world_salt>(ecs::world_salt{ 1 });
	entt::organizer org;
	factory.create_system("w", r0, org);
	factory.create_system("w", r1, org);
	auto graph = org.graph();
	BOOST_REQUIRE(graph.size() == 2u);
	const entt::type_info* w0[2]{};
	const entt::type_info* w1[2]{};
	BOOST_REQUIRE(graph[0].rw_dependency(w0, 2) == 1u);
	BOOST_REQUIRE(graph[1].rw_dependency(w1, 2) == 1u);
	BOOST_TEST(w0[0]->hash() != w1[0]->hash()); // the same resource is a different dependency per world
	BOOST_TEST(w0[0]->hash() == (w1[0]->hash() ^ (entt::id_type{ 1 } << 16))); // salt = world id << 16
}

BOOST_AUTO_TEST_SUITE_END()
