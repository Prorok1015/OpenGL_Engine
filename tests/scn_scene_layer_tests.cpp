#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/ecs/ecs_command_buffer.hpp>
#include <snk/ecs/ecs_component.h>
#include <snk/ecs/ecs_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/scn_registration_scope.h>
#include <snk/scene/scn_scene_layer.h>
#include <boost/json.hpp>
#include <entt/entt.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

// The scene layer (#220 PR-5, MODULE_LAYER.md): the game worlds are sets of a layer and are built by its services,
// a component of the layer is found through its meta context, and destroying the layer takes its worlds with it
// and leaves the base as it was.

namespace {

struct game_component {
	int value = 0;
};

struct game_desc : desc::desc_base {
	int value = 0;

	void deserialize(desc::desc_system&, const boost::json::object& obj) override
	{
		if (const auto* v = obj.if_contains("value")) value = static_cast<int>(v->to_number<std::int64_t>());
	}
	void serialize(boost::json::object& obj) const override { obj["value"] = value; }
};

void game_move(entt::view<entt::get_t<game_component>> view)
{
	for (auto&& [entity, component] : view.each()) {
		++component.value;
	}
}

constexpr const char* GAME_LEVEL = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":["game::move"],
	 "components":{"g":{"__type":"game_desc","value":2}}}]})";

constexpr entt::id_type game_hash = entt::type_hash<game_component>::value();

struct layer_fixture : test_util::scene_env {
	layer_fixture()
		: hot(std::make_shared<scn::hot_reload_manager>(*assembler, *rs))
		, mgr(*rs, *assembler, sf, hot)
	{
		layer = &mgr.create_layer();
		mgr.set_game_layer(layer);
		game.emplace(layer->descs(), layer->assembler());
		game->desc<game_desc>("game_desc", true);
		game->spawner("game_desc", [](entt::registry& reg, entt::entity e, const desc::desc_base& base, const std::string_view) {
			reg.emplace_or_replace<game_component>(e, game_component{ static_cast<const game_desc&>(base).value });
		});
		layer->systems().register_system<&game_move>("game::move");
		ecs::register_component<game_component>(layer->meta());
	}

	// Releases what the module would release before its layer goes: its desc registrations, then the layer.
	void destroy_layer()
	{
		game.reset();
		mgr.destroy_layer(*layer);
		layer = nullptr;
	}

	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	scn::context_manager mgr;
	scn::scene_layer* layer = nullptr;
	std::optional<scn::registration_scope> game;
};

}

BOOST_FIXTURE_TEST_SUITE(scn_scene_layer_tests, layer_fixture)

BOOST_AUTO_TEST_CASE(the_level_of_the_game_layer_is_built_by_its_services)
{
	BOOST_TEST(mgr.game_layer() == layer);
	BOOST_TEST(mgr.level_set().layer() == layer);
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", GAME_LEVEL)));
	pump();

	auto* world = mgr.level_set().find("w0");
	BOOST_REQUIRE(world != nullptr);
	auto view = world->state().view<game_component>();
	BOOST_REQUIRE(view.size() == 1u);
	BOOST_TEST(view.get<game_component>(view.front()).value == 2);

	mgr.update(std::chrono::duration<float>(0.016f));
	BOOST_TEST(view.get<game_component>(view.front()).value == 3); // the layer's system ran

	BOOST_TEST(!descs->is_registered("game_desc"));
	BOOST_TEST(!sf.registry().find("game::move"));
	BOOST_TEST(layer->resources().live_resources() > 0u); // the level desc of the layer is in the layer's cache
}

BOOST_AUTO_TEST_CASE(sandbox_components_of_the_layer_are_found_in_its_meta_context)
{
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", GAME_LEVEL)));
	pump();
	auto& world = mgr.level_set().get("w0");
	if (!world.state().ctx().contains<ecs::entity_spawner>()) {
		world.state().ctx().emplace<ecs::entity_spawner>();
	}
	auto& sandbox = world.state().ctx().get<ecs::entity_spawner>();
	const auto spawned = sandbox.create();
	sandbox.emplace<game_component>(spawned, game_component{ 40 });

	world.apply_commands();
	BOOST_TEST(world.state().view<game_component>().size() == 2u);
	BOOST_TEST(!entt::resolve(ecs::meta_context(), game_hash)); // only the layer knows it
	BOOST_TEST(static_cast<bool>(entt::resolve(layer->meta(), game_hash)));
}

BOOST_AUTO_TEST_CASE(a_module_attaches_to_the_meta_context_of_its_layer)
{
	BOOST_TEST(&layer->meta() != &ecs::meta_context());
	// In a static build this binary's locator is the engine's: put it back before anything else resolves.
	const auto saved = entt::locator<entt::meta_ctx>::handle();
	entt::locator<entt::meta_ctx>::reset(layer->meta_handle());
	BOOST_TEST(&entt::locator<entt::meta_ctx>::value_or() == &layer->meta());
	entt::locator<entt::meta_ctx>::reset(saved);
}

BOOST_AUTO_TEST_CASE(previews_belong_to_the_game_layer)
{
	mgr.create_preview("memory://t/p.prefab.desc", "preview");
	const auto* set = mgr.find_set(scn::context_manager::preview_set_name("memory://t/p.prefab.desc"));
	BOOST_REQUIRE(set != nullptr);
	BOOST_TEST(set->layer() == layer);
}

BOOST_AUTO_TEST_CASE(destroying_the_layer_takes_its_worlds_and_leaves_the_base)
{
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", GAME_LEVEL)));
	pump();
	mgr.create_preview("memory://t/p.prefab.desc", "preview");
	const auto base_components = descs->get_component_type_names();

	destroy_layer();
	BOOST_TEST(mgr.game_layer() == nullptr);
	BOOST_TEST(mgr.level_set().layer() == nullptr);
	BOOST_TEST(mgr.level_set().size() == 0u);
	BOOST_TEST(!mgr.find_preview("memory://t/p.prefab.desc"));
	BOOST_TEST(descs->get_component_type_names() == base_components, boost::test_tools::per_element());
	BOOST_TEST(!assembler->has_spawner("game_desc"));

	// The base still loads a level of its own types.
	BOOST_REQUIRE(mgr.load_level(put("base.desc", R"({"__type":"level_desc","name":"lv","worlds":[{"__type":"world_desc","name":"b","systems":[]}]})")));
	pump();
	BOOST_TEST(mgr.level_set().has("b"));
}

BOOST_AUTO_TEST_CASE(a_new_layer_registers_the_same_names_again)
{
	destroy_layer();
	layer = &mgr.create_layer();
	mgr.set_game_layer(layer);
	game.emplace(layer->descs(), layer->assembler());
	game->desc<game_desc>("game_desc", true);
	BOOST_TEST(layer->systems().register_system<&game_move>("game::move"));
	BOOST_TEST(layer->descs().is_registered("game_desc"));
}

BOOST_AUTO_TEST_SUITE_END()
