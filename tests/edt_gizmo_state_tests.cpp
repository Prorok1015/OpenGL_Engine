#include <boost/test/unit_test.hpp>
#include <entt/entt.hpp>
#include "edt_gizmo_state.h"
#include <optional>

// #728: the entity ids the viewport and its gizmo hold mean something only in the registry they were taken from. A world
// that is rebuilt gets a new registry, often at the address of the old one and with the same ids.

BOOST_AUTO_TEST_SUITE(EdtGizmoStateTests)

BOOST_AUTO_TEST_CASE(TheRotateRing_DoesNotSurviveARegistryMadeAtTheAddressOfAGoneOne_EvenWithTheSameEntityId)
{
	std::optional<entt::registry> world;
	world.emplace();
	const auto ent = world->create();
	edt::rotate_gizmo_state state;
	state.sync(ent, edt::registry_token(*world));
	state.ring = 1;
	state.sync(ent, edt::registry_token(*world));
	BOOST_TEST(state.ring == 1);

	const entt::registry* address = &*world;
	world.reset();
	world.emplace();
	BOOST_REQUIRE(&*world == address);
	const auto same_id = world->create();
	BOOST_REQUIRE((same_id == ent));
	state.sync(same_id, edt::registry_token(*world));
	BOOST_TEST(state.ring == -1);
}

BOOST_AUTO_TEST_CASE(TheRotateRing_IsKeptForTheSameEntityAndRegistry_AndEndedByAnotherEntity)
{
	entt::registry world;
	const auto first = world.create();
	const auto second = world.create();
	const std::uint64_t token = edt::registry_token(world);
	edt::rotate_gizmo_state state;

	state.sync(first, token);
	BOOST_TEST(state.ring == -1);
	state.ring = 2;
	state.sync(first, token);
	BOOST_TEST(state.ring == 2);
	state.sync(second, token);
	BOOST_TEST(state.ring == -1);
}

BOOST_AUTO_TEST_CASE(TheRotateRing_DoesNotSurviveClearOfItsRegistry_WhoseEntitiesComeBackWithANewVersion)
{
	entt::registry world;
	const auto ent = world.create();
	const std::uint64_t token = edt::registry_token(world);
	edt::rotate_gizmo_state state;
	state.sync(ent, token);
	state.ring = 0;

	world.clear();
	const auto back = world.create();
	// clear() leaves the ctx, so the token is the same; the entity value is not.
	BOOST_REQUIRE(edt::registry_token(world) == token);
	BOOST_REQUIRE(!(back == ent));
	state.sync(back, edt::registry_token(world));
	BOOST_TEST(state.ring == -1);
}

BOOST_AUTO_TEST_CASE(AResetStateStartsOver)
{
	entt::registry world;
	const auto ent = world.create();
	edt::rotate_gizmo_state state;
	state.sync(ent, edt::registry_token(world));
	state.ring = 1;
	state.reset();
	BOOST_TEST(state.ring == -1);
	BOOST_TEST((state.ent == entt::null));
	BOOST_TEST(state.token == 0u);
}

BOOST_AUTO_TEST_CASE(TheViewportSelection_IsTakenFromTheResolverWhenTheRegistryIsAnotherOne_AndKeptWhileItIsNot)
{
	std::optional<entt::registry> world;
	world.emplace();
	const auto a = world->create();
	const auto b = world->create();
	int asked = 0;
	entt::entity answer = b;
	edt::viewport_selection selection;
	selection.set_resolver([&] {
		++asked;
		return answer;
	});

	BOOST_TEST(selection.refresh(&*world));
	BOOST_TEST(asked == 1);
	BOOST_TEST((selection.get() == b));

	// An event of the selection names the entity; the same registry keeps it and asks nobody.
	selection.set(a);
	BOOST_TEST(!selection.refresh(&*world));
	BOOST_TEST(asked == 1);
	BOOST_TEST((selection.get() == a));

	// The world was rebuilt at the same address and the old pick has the id of the first entity there.
	const entt::registry* address = &*world;
	world.reset();
	world.emplace();
	BOOST_REQUIRE(&*world == address);
	const auto x0 = world->create();
	const auto x1 = world->create();
	BOOST_REQUIRE((x0 == a));
	answer = x1;
	BOOST_TEST(selection.refresh(&*world));
	BOOST_TEST(asked == 2);
	BOOST_TEST((selection.get() == x1));
	BOOST_TEST(!(selection.get() == a));
}

BOOST_AUTO_TEST_CASE(TheViewportSelection_IsEmptyWhenTheResolverGivesAnEntityTheRegistryDoesNotHold)
{
	entt::registry world;
	(void)world.create();
	entt::entity answer = static_cast<entt::entity>(99);
	edt::viewport_selection selection;
	selection.set_resolver([&] { return answer; });

	BOOST_TEST(selection.refresh(&world));
	BOOST_TEST((selection.get() == entt::null));

	entt::registry other;
	answer = entt::null;
	BOOST_TEST(selection.refresh(&other));
	BOOST_TEST((selection.get() == entt::null));
}

BOOST_AUTO_TEST_CASE(TheViewportSelection_WithoutAResolver_IsEmptyForAnotherRegistry)
{
	entt::registry first;
	entt::registry second;
	const auto ent = first.create();
	edt::viewport_selection selection;
	BOOST_TEST(selection.refresh(&first));
	selection.set(ent);
	BOOST_TEST(!selection.refresh(&first));
	BOOST_TEST((selection.get() == ent));

	BOOST_TEST(selection.refresh(&second));
	BOOST_TEST((selection.get() == entt::null));
}

BOOST_AUTO_TEST_CASE(TheViewportSelection_IsEmptyWithoutARegistry_AndSaysSoOnlyOnce)
{
	entt::registry world;
	const auto ent = world.create();
	edt::viewport_selection selection;
	selection.set_resolver([&] { return ent; });
	selection.refresh(&world);
	BOOST_REQUIRE((selection.get() == ent));

	BOOST_TEST(selection.refresh(nullptr));
	BOOST_TEST((selection.get() == entt::null));
	BOOST_TEST(!selection.refresh(nullptr));
}

BOOST_AUTO_TEST_CASE(TheViewportSelection_TakesTheEntityAgainWhenItsRegistryWasCleared)
{
	entt::registry world;
	const auto a = world.create();
	const auto b = world.create();
	entt::entity answer = b;
	edt::viewport_selection selection;
	selection.set_resolver([&] { return answer; });
	selection.refresh(&world);
	selection.set(b);

	world.clear();
	const auto x0 = world.create();
	const auto x1 = world.create();
	BOOST_REQUIRE(!world.valid(b));
	BOOST_REQUIRE(!world.valid(a));
	answer = x1;
	BOOST_TEST(selection.refresh(&world));
	BOOST_TEST((selection.get() == x1));
	(void)x0;
}

BOOST_AUTO_TEST_CASE(ARegistryThatIsNotTheOneSeenLast_EndsADragOfTheRotateRing_AndAnEntityOfItsOwnKeepsIt)
{
	std::optional<entt::registry> world;
	world.emplace();
	const auto ent = world->create();
	edt::viewport_selection selection;
	selection.set_resolver([&] { return ent; });
	edt::rotate_gizmo_state rotate;

	const auto frame = [&] {
		if (selection.refresh(&*world)) rotate.reset();
		rotate.sync(selection.get(), edt::registry_token(*world));
	};
	frame();
	rotate.ring = 1;
	frame();
	BOOST_TEST(rotate.ring == 1);

	world.reset();
	world.emplace();
	const auto same_id = world->create();
	BOOST_REQUIRE((same_id == ent));
	frame();
	BOOST_TEST(rotate.ring == -1);
}

BOOST_AUTO_TEST_SUITE_END()
