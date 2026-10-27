#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/desc/desc_system.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/scn_registration_scope.h>
#include <boost/json.hpp>
#include <entt/entt.hpp>
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

// Layered desc system and assembler (#220 PR-3, MODULE_LAYER.md): the base holds the scene types, a
// layer adds a game desc type with its spawner, reads the base's types, schemas, migrations and
// spawners, and cannot change them.

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

struct layer_fixture : test_util::ram_res_env {
	layer_fixture()
		: base_descs(*rs)
		, base_assembler(base_descs, *rs)
		, base_scope(scn::register_scene_types(base_descs, base_assembler, skinning))
	{
		layer_descs.emplace(*rs, &base_descs);
		layer_assembler.emplace(*layer_descs, *rs, &base_assembler);
		layer_scope.emplace(*layer_descs, *layer_assembler);
		layer_scope->desc<game_desc>("game_desc", true);
		layer_scope->spawner("game_desc", [](entt::registry& reg, entt::entity e, const desc::desc_base& base, const std::string_view) {
			reg.emplace_or_replace<game_component>(e, game_component{ static_cast<const game_desc&>(base).value });
		});
	}

	~layer_fixture()
	{
		layer_scope.reset();
		layer_assembler.reset();
		layer_descs.reset();
	}

	static bool has_name(const std::vector<std::string>& list, std::string_view name)
	{
		return std::ranges::find(list, name) != list.end();
	}

	desc::desc_system base_descs;
	scn::ecs_assembler base_assembler;
	rnd::skinning_manager skinning;
	scn::registration_scope base_scope;

	std::optional<desc::desc_system> layer_descs;
	std::optional<scn::ecs_assembler> layer_assembler;
	std::optional<scn::registration_scope> layer_scope;
};

}

BOOST_FIXTURE_TEST_SUITE(scn_layered_desc_tests, layer_fixture)

BOOST_AUTO_TEST_CASE(layer_reads_base_types_and_adds_its_own)
{
	BOOST_TEST(layer_descs->is_registered("camera_desc"));
	BOOST_TEST(layer_descs->is_registered("game_desc"));
	BOOST_TEST(!base_descs.is_registered("game_desc"));
	BOOST_TEST(static_cast<bool>(layer_descs->find_schema("camera_desc")));
	BOOST_TEST(has_name(layer_descs->schemas().type_names(), "camera_desc"));

	const auto layer_components = layer_descs->get_component_type_names();
	BOOST_TEST(layer_components.size() == base_descs.get_component_type_names().size() + 1);
	BOOST_TEST(has_name(layer_components, "camera_desc"));
	BOOST_TEST(has_name(layer_components, "game_desc"));
	BOOST_TEST(!has_name(base_descs.get_component_type_names(), "game_desc"));

	BOOST_TEST(static_cast<bool>(layer_descs->create_instance("camera_desc", res::tag{})));
	BOOST_TEST(!base_descs.create_instance("game_desc", res::tag{}));
}

BOOST_AUTO_TEST_CASE(layer_cannot_register_or_migrate_a_base_type)
{
	layer_descs->register_desc<game_desc>("camera_desc");
	const auto camera = layer_descs->create_instance("camera_desc", res::tag{});
	BOOST_TEST(!dynamic_cast<game_desc*>(camera.get())); // the base's factory still answers

	const int version = layer_descs->current_version("prefab_desc");
	BOOST_TEST(version == base_descs.current_version("prefab_desc"));
	layer_descs->register_migration("prefab_desc", version, [](json::object&) {});
	BOOST_TEST(layer_descs->current_version("prefab_desc") == version);
	BOOST_TEST(base_descs.current_version("prefab_desc") == version);

	BOOST_TEST(!layer_assembler->register_desc_spawner("camera_desc", [](entt::registry&, entt::entity, const desc::desc_base&, const std::string_view) {}));
}

BOOST_AUTO_TEST_CASE(layer_runs_the_base_migrations)
{
	auto obj = boost::json::parse(R"({ "__type": "prefab_desc", "__version": 1, "children": {} })").as_object();
	layer_descs->migrate_tree(obj, "test");
	BOOST_TEST(obj.at("__version").as_int64() == base_descs.current_version("prefab_desc"));
	BOOST_TEST(obj.at("children").is_array());
}

BOOST_AUTO_TEST_CASE(layer_assembler_spawns_layer_and_base_types)
{
	BOOST_TEST(layer_assembler->has_spawner("camera_desc"));
	BOOST_TEST(layer_assembler->has_spawner("game_desc"));
	BOOST_TEST(!base_assembler.has_spawner("game_desc"));

	auto game = layer_descs->create_instance("game_desc", res::tag{});
	BOOST_REQUIRE(game);
	game->deserialize(*layer_descs, boost::json::parse(R"({ "value": 5 })").as_object());

	entt::registry reg;
	const auto e = reg.create();
	layer_assembler->spawn_from_desc(reg, e, *game, "game");
	BOOST_REQUIRE(reg.all_of<game_component>(e));
	BOOST_TEST(reg.get<game_component>(e).value == 5);

	const auto other = reg.create();
	base_assembler.spawn_from_desc(reg, other, *game, "game"); // the base has no spawner for it
	BOOST_TEST(!reg.all_of<game_component>(other));
}

BOOST_AUTO_TEST_CASE(base_is_unchanged_after_the_layer_goes_away)
{
	const auto components = base_descs.get_component_type_names();
	const auto schemas = base_descs.schemas().type_names();
	layer_scope.reset();
	layer_assembler.reset();
	layer_descs.reset();

	BOOST_TEST(base_descs.get_component_type_names() == components, boost::test_tools::per_element());
	BOOST_TEST(base_descs.schemas().type_names() == schemas, boost::test_tools::per_element());
	BOOST_TEST(!base_descs.is_registered("game_desc"));
	BOOST_TEST(!base_assembler.has_spawner("game_desc"));
	BOOST_TEST(base_assembler.has_spawner("camera_desc"));
}

BOOST_AUTO_TEST_SUITE_END()
