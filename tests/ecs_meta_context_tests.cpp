#include <boost/test/unit_test.hpp>
#include <snk/ecs/ecs_command_buffer.hpp>
#include <snk/ecs/ecs_component.h>
#include <snk/ecs/ecs_meta_context.h>
#include <snk/scene/level/scn_world_context.h>
#include <entt/entt.hpp>
#include <cstdint>
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

	struct pointer_component {
		entt::entity target = entt::null;
	};

	struct tag_component {};

	// Both kinds of entity field a C++ component may hold (#738).
	struct linked_component {
		entt::entity world_target = entt::null;
		ecs::sandbox_entity sandbox_target = entt::null;
		int value = 0;
	};

	entt::entity sandbox_id(ecs::sandbox_entity entity)
	{
		return static_cast<entt::entity>(static_cast<std::uint32_t>(entity));
	}

	std::size_t live_entities(const entt::registry& reg)
	{
		const auto* entities = reg.storage<entt::entity>(); // a const registry gives the storage by pointer
		return entities ? entities->free_list() : 0u;
	}

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

// #708: the entity pass moved only the first sandbox entity; one without components never reached the world and a
// reference to it became null.
BOOST_AUTO_TEST_CASE(an_entity_without_components_reaches_the_world_and_a_reference_to_it_is_mapped)
{
	using namespace entt::literals;
	entt::meta_ctx layer;
	ecs::register_component<pointer_component>(layer).data<&pointer_component::target>("target"_hs);

	entt::registry reg;
	// The world's ids differ from the sandbox's: an unmapped raw id would name one of these.
	const entt::entity w0 = reg.create();
	const entt::entity w1 = reg.create();
	auto& sandbox = reg.ctx().emplace<ecs::entity_spawner>();
	// The bare entity first: the old entity pass mapped only the last packed one, which the pool maps anyway.
	const auto bare = sandbox.create();
	const auto pointer = sandbox.create();
	sandbox.emplace<pointer_component>(pointer, pointer_component{ sandbox_id(bare) });

	scn::world_context::apply_commands(reg, &layer);

	BOOST_TEST(live_entities(reg) == 4u);
	const auto view = reg.view<const pointer_component>();
	BOOST_REQUIRE(view.size() == 1u);
	const entt::entity moved_pointer = view.front();
	const entt::entity target = view.get<const pointer_component>(moved_pointer).target;
	BOOST_TEST((target != entt::null));
	BOOST_TEST(reg.valid(target));
	BOOST_TEST((target != w0 && target != w1));
	BOOST_TEST((target != moved_pointer));
	BOOST_TEST(!reg.all_of<pointer_component>(target));
}

// #738: the entity fields of a C++ component are remapped by the meta of the context it is registered in. Nothing is
// registered in the default context: there remap_fields looked, and found no fields.
BOOST_AUTO_TEST_CASE(entity_fields_are_remapped_by_the_context_of_the_registration)
{
	using namespace entt::literals;
	BOOST_REQUIRE(!entt::resolve<linked_component>());
	const auto register_linked = [](entt::meta_ctx& ctx) {
		ecs::register_component<linked_component>(ctx)
			.data<&linked_component::world_target>("world_target"_hs)
			.data<&linked_component::sandbox_target>("sandbox_target"_hs)
			.data<&linked_component::value>("value"_hs);
	};

	entt::meta_ctx layer;
	register_linked(layer);
	register_linked(ecs::mutable_meta_context());
	for (const entt::meta_ctx* context : { static_cast<const entt::meta_ctx*>(&layer), static_cast<const entt::meta_ctx*>(nullptr) }) {
		BOOST_TEST_INFO((context ? "layer" : "engine"));
		entt::registry reg;
		static_cast<void>(reg.create());
		static_cast<void>(reg.create());
		auto& sandbox = reg.ctx().emplace<ecs::entity_spawner>();
		const auto first = sandbox.create();
		const auto second = sandbox.create();
		sandbox.emplace<linked_component>(first, linked_component{ sandbox_id(second), second, 3 });
		sandbox.emplace<linked_component>(second, linked_component{ sandbox_id(first), first, 4 });

		scn::world_context::apply_commands(reg, context);

		const auto view = reg.view<const linked_component>();
		BOOST_REQUIRE(view.size() == 2u);
		for (const entt::entity entity : view) {
			const linked_component& linked = view.get<const linked_component>(entity);
			BOOST_TEST(reg.valid(linked.world_target));
			BOOST_TEST((linked.world_target != entity));
			BOOST_TEST((linked.world_target == ecs::to_main(linked.sandbox_target)));
			BOOST_TEST(view.get<const linked_component>(linked.world_target).value == (linked.value == 3 ? 4 : 3));
		}
	}
	entt::meta_reset<linked_component>(ecs::mutable_meta_context());
	BOOST_TEST(!entt::resolve<linked_component>());
}

// The ids the sandbox released on an earlier apply are not live: they make no world entity.
BOOST_AUTO_TEST_CASE(released_sandbox_ids_make_no_world_entity)
{
	entt::registry reg;
	auto& sandbox = reg.ctx().emplace<ecs::entity_spawner>();
	static_cast<void>(sandbox.create());
	static_cast<void>(sandbox.create());
	scn::world_context::apply_commands(reg);
	BOOST_TEST(live_entities(reg) == 2u);

	static_cast<void>(sandbox.create());
	scn::world_context::apply_commands(reg);
	BOOST_TEST(live_entities(reg) == 3u);
}

// An empty component has no value to read: the entity read alone must advance, or every tag lands on one entity.
BOOST_AUTO_TEST_CASE(a_tag_lands_on_every_entity_that_has_it)
{
	entt::meta_ctx layer;
	ecs::register_component<tag_component>(layer);
	entt::registry reg;
	auto& sandbox = reg.ctx().emplace<ecs::entity_spawner>();
	for (int i = 0; i < 3; ++i) {
		sandbox.emplace<tag_component>(sandbox.create());
	}

	scn::world_context::apply_commands(reg, &layer);

	BOOST_TEST(live_entities(reg) == 3u);
	BOOST_TEST(reg.storage<tag_component>().size() == 3u);
}

BOOST_AUTO_TEST_SUITE_END()
