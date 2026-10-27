#include <boost/test/unit_test.hpp>
#include "test_module_env.hpp"
#include <snk/ecs/ecs_component.h>
#include <snk/ecs/ecs_system.h>
#include <snk/game/gs_game_module.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_scene_layer.h>
#include <boost/json.hpp>
#include <entt/entt.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// A game module in the same binary (#220 PR-6, MODULE_LAYER.md): module_loader gives it a layer, it registers
// a desc type with its spawner, a system, a component's meta and a service of its own there, and unloading it
// leaves the root storage and the base registries as they were.

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

struct game_service {
	int frames = 0;
};

void game_move(entt::view<entt::get_t<game_component>> view)
{
	for (auto&& [entity, component] : view.each()) {
		++component.value;
	}
}

class test_game_module final : public gs::game_module {
public:
	std::string_view name() const override { return "test_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage& data) override
	{
		layer.descs().register_component_desc<game_desc>("game_desc");
		layer.assembler().register_desc_spawner("game_desc", [](entt::registry& reg, entt::entity e, const desc::desc_base& base, const std::string_view) {
			reg.emplace_or_replace<game_component>(e, game_component{ static_cast<const game_desc&>(base).value });
		});
		layer.systems().register_system<&game_move>("game::move");
		layer.components().add<game_component>("game.component");
		ecs::register_component<game_component>(layer.meta());
		data.construct<game_service>();
	}

	void shutdown_content(scn::scene_layer&, ds::app_data_storage& data) override
	{
		data.destruct<game_service>();
	}
};

constexpr const char* GAME_LEVEL = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":["game::move"],
	 "components":{"g":{"__type":"game_desc","value":2}}}]})";

}

BOOST_FIXTURE_TEST_SUITE(app_game_module_tests, test_util::module_env)

BOOST_AUTO_TEST_CASE(a_game_module_registers_into_its_layer_and_its_worlds_run)
{
	const std::size_t root_size = root.size();
	loader.load(std::make_unique<test_game_module>(), root);
	BOOST_TEST(root.size() == root_size); // its service is in the layer's storage

	auto* layer = contexts().game_layer();
	BOOST_REQUIRE(layer != nullptr);
	BOOST_TEST(layer->descs().is_registered("game_desc"));
	BOOST_TEST(!descs->is_registered("game_desc"));
	BOOST_TEST(!sf.registry().find("game::move"));
	BOOST_TEST(!root.has_value<game_service>());

	BOOST_REQUIRE(contexts().load_level(put("lv.desc", GAME_LEVEL)));
	pump();
	auto* world = contexts().level_set().find("w0");
	BOOST_REQUIRE(world != nullptr);
	auto view = world->state().view<game_component>();
	BOOST_REQUIRE(view.size() == 1u);
	contexts().update(std::chrono::duration<float>(0.016f));
	BOOST_TEST(view.get<game_component>(view.front()).value == 3);
}

BOOST_AUTO_TEST_CASE(unloading_leaves_the_root_and_the_base_as_they_were)
{
	const std::size_t root_size = root.size();
	const auto base_components = descs->get_component_type_names();
	loader.load(std::make_unique<test_game_module>(), root);
	BOOST_REQUIRE(contexts().load_level(put("lv.desc", GAME_LEVEL)));
	pump();
	auto* module_layer = contexts().game_layer();

	loader.unload("test_game", root);
	BOOST_TEST(root.size() == root_size);
	BOOST_TEST((loader.state("test_game") == app::modules::module_state::unloaded));
	BOOST_TEST(descs->get_component_type_names() == base_components, boost::test_tools::per_element());
	BOOST_TEST(!assembler->has_spawner("game_desc"));

	// A fresh, empty game layer is there for the editor; it knows nothing of the module.
	auto* fresh = contexts().game_layer();
	BOOST_REQUIRE(fresh != nullptr);
	BOOST_TEST(fresh != module_layer);
	BOOST_TEST(!fresh->descs().is_registered("game_desc"));
	BOOST_TEST(contexts().level_set().size() == 0u);
}

BOOST_AUTO_TEST_CASE(the_module_loads_again_after_unloading)
{
	for (int round = 0; round < 2; ++round) {
		BOOST_TEST_CONTEXT("round " << round) {
			loader.load(std::make_unique<test_game_module>(), root);
			BOOST_TEST(contexts().game_layer()->descs().is_registered("game_desc"));
			BOOST_TEST((contexts().game_layer()->components().find("game.component") == entt::type_hash<game_component>::value()));
			loader.unload("test_game", root);
		}
	}
}

BOOST_AUTO_TEST_CASE(a_layered_module_needs_a_layer_factory)
{
	root.destruct<app::modules::module_layer_factory>();
	BOOST_CHECK_THROW(loader.load(std::make_unique<test_game_module>(), root), std::logic_error);
	BOOST_TEST(!loader.state("test_game").has_value());
}

BOOST_AUTO_TEST_SUITE_END()
