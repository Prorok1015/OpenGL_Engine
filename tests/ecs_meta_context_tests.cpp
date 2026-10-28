#include <boost/test/unit_test.hpp>
#include <snk/ecs/ecs_command_buffer.hpp>
#include <snk/ecs/ecs_component.h>
#include <snk/ecs/ecs_meta_context.h>
#include <snk/scene/level/scn_world_context.h>
#include <entt/entt.hpp>
#include <type_traits>

// The engine's meta context (#220 PR-1, spike #218): components registered there are what
// world_context::apply_commands moves from the sandbox into the world.

namespace {

	struct registered_component {
		int value = 0;
	};

	struct unregistered_component {
		int value = 0;
	};

	constexpr entt::id_type registered_hash = entt::type_hash<registered_component>::value();
	constexpr entt::id_type unregistered_hash = entt::type_hash<unregistered_component>::value();

	struct meta_fixture {
		meta_fixture() { ecs::register_component<registered_component>(ecs::mutable_meta_context()); }
		~meta_fixture() { entt::meta_reset<registered_component>(ecs::mutable_meta_context()); }

		meta_fixture(const meta_fixture&) = delete;
		meta_fixture& operator=(const meta_fixture&) = delete;
	};

}

BOOST_AUTO_TEST_SUITE(ecs_meta_context_tests)

// What a module sees of the engine's context is read-only (#408 review).
static_assert(std::is_same_v<decltype(ecs::meta_context()), const entt::meta_ctx&>);

BOOST_AUTO_TEST_CASE(attaching_the_locator_shares_the_engine_context)
{
	const auto saved = entt::locator<entt::meta_ctx>::handle();
	entt::locator<entt::meta_ctx>::reset(ecs::meta_context_handle());
	BOOST_TEST(&entt::locator<entt::meta_ctx>::value_or() == &ecs::meta_context());
	entt::locator<entt::meta_ctx>::reset(saved);
}

BOOST_FIXTURE_TEST_CASE(registered_component_is_resolved_by_its_type_hash, meta_fixture)
{
	const auto type = entt::resolve(ecs::meta_context(), registered_hash);
	BOOST_REQUIRE(type);
	BOOST_TEST(type.id() == registered_hash);
	BOOST_TEST(static_cast<bool>(type.func(ecs::snapshot_get_h)));
	BOOST_TEST(static_cast<bool>(type.func(ecs::loader_get_h)));
	BOOST_TEST(!entt::resolve(ecs::meta_context(), unregistered_hash));
}

// A module registers into its layer's context (scene_layer::meta()), never into the engine's (#405).
BOOST_AUTO_TEST_CASE(a_registration_in_a_layer_context_stays_out_of_the_engine_context)
{
	struct layer_component {};
	constexpr entt::id_type id = entt::type_hash<layer_component>::value();
	entt::meta_ctx layer;
	ecs::register_component<layer_component>(layer);
	BOOST_TEST(static_cast<bool>(entt::resolve(layer, id)));
	BOOST_TEST(!entt::resolve(ecs::meta_context(), id));
	BOOST_TEST(static_cast<bool>(ecs::find_meta(&layer, id)));
	BOOST_TEST(!ecs::find_meta(nullptr, id));
}

BOOST_FIXTURE_TEST_CASE(meta_reset_removes_the_registration, meta_fixture)
{
	entt::meta_reset<registered_component>(ecs::mutable_meta_context());
	BOOST_TEST(!entt::resolve(ecs::meta_context(), registered_hash));
}

BOOST_FIXTURE_TEST_CASE(apply_commands_moves_only_registered_components, meta_fixture)
{
	entt::registry reg;
	auto& sandbox = reg.ctx().emplace<ecs::entity_spawner>();
	const auto spawned = sandbox.create();
	sandbox.emplace<registered_component>(spawned, registered_component{ 7 });
	sandbox.emplace<unregistered_component>(spawned, unregistered_component{ 9 });

	scn::world_context::apply_commands(reg);

	const auto* moved = reg.storage(registered_hash);
	BOOST_REQUIRE(moved != nullptr);
	BOOST_TEST(moved->size() == 1u);
	for (auto&& [entity, component] : reg.view<const registered_component>().each()) {
		BOOST_TEST(component.value == 7);
	}
	BOOST_TEST(!reg.storage(unregistered_hash));
}

BOOST_AUTO_TEST_SUITE_END()
