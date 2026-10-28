#include <boost/test/unit_test.hpp>
#include "test_module_env.hpp"
#include "edt_level_controller.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include <snk/editor/edt_editor_services.h>
#include <snk/game/gs_game_module.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/scn_scene_layer.h>
#include <boost/json.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The editor follows a change of the game layer (#220, review of #401): loading or unloading a game module replaces
// the layer the edited level lives in, and the editor lets go of the old one and rebuilds the level in the new one
// (what editor_system wires to context_manager::set_on_game_layer_change).

namespace {

struct editor_game_desc : desc::desc_base {
	void deserialize(desc::desc_system&, const boost::json::object&) override {}
	void serialize(boost::json::object&) const override {}
};

class editor_game_module final : public gs::game_module {
public:
	std::string_view name() const override { return "editor_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage&) override
	{
		layer.descs().register_component_desc<editor_game_desc>("editor_game_desc");
	}
};

// A level with a component of the module's desc type.
constexpr const char* MODULE_LEVEL = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":[],"components":{"gc":{"__type":"editor_game_desc","speed":3}}}]})";

constexpr const char* BASE_LEVEL = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":[]},
	{"__type":"world_desc","name":"w1","systems":[]}]})";

struct editor_fixture : test_util::module_env {
	editor_fixture()
	{
		services.ctx_manager = root.require_shared<scn::context_manager>();
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;
		contexts().set_on_game_layer_change([this](scn::context_manager::game_layer_change change) {
			if (change == scn::context_manager::game_layer_change::before) {
				level_ctrl.detach_from_game_layer();
			} else {
				module_types_at_attach.push_back(services.descs()->is_registered("editor_game_desc"));
				level_ctrl.attach_to_game_layer();
			}
		});
	}

	~editor_fixture()
	{
		contexts().set_on_game_layer_change(nullptr);
	}

	void open_level(const res::tag& tag)
	{
		state.editor_tag = tag;
		state.level_tag = tag;
		auto handle = services.resources()->require_sync<scn::level_desc>(tag);
		level_ctrl.finish_level_load(handle);
	}

	edt::editor_services services;
	edt::scene_document state;
	edt::scene_editor scene_ed{ state, services };
	edt::world_controller world_ctrl{ state, services };
	edt::level_controller level_ctrl{ state, services, scene_ed, world_ctrl };
	// Whether the module's desc type was known each time the editor rebuilt its level.
	std::vector<bool> module_types_at_attach;

	bool level_has_module_component() const
	{
		return boost::json::serialize(level_ctrl.build_level_json()).find("editor_game_desc") != std::string::npos;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(edt_game_layer_change_tests, editor_fixture)

BOOST_AUTO_TEST_CASE(the_edited_level_moves_into_the_layer_of_a_loaded_module_and_back)
{
	contexts().level_set().set_mode(ecs::run_mode::edit);
	open_level(put("lv.desc", BASE_LEVEL));
	BOOST_REQUIRE((state.world_names == std::vector<std::string>{ "w0", "w1" }));
	world_ctrl.select_world(1);
	BOOST_REQUIRE(state.active_world_idx == 1);
	auto* empty_layer = contexts().game_layer();

	loader.load(std::make_unique<editor_game_module>(), root);
	auto* module_layer = contexts().game_layer();
	BOOST_TEST(module_layer != empty_layer);
	BOOST_TEST(contexts().level_set().layer() == module_layer);
	BOOST_TEST(contexts().level_set().find("w0") != nullptr);
	BOOST_TEST(contexts().level_set().find("w1") != nullptr);
	BOOST_TEST((contexts().level_set().mode() == ecs::run_mode::edit));
	BOOST_TEST((state.world_names == std::vector<std::string>{ "w0", "w1" }), boost::test_tools::per_element());
	BOOST_TEST(state.world_descs.size() == 2u);
	BOOST_TEST(state.active_world_idx == 1);
	BOOST_TEST(services.descs() == &module_layer->descs());
	BOOST_TEST(services.descs()->is_registered("editor_game_desc"));

	loader.unload("editor_game", root);
	auto* fresh = contexts().game_layer();
	BOOST_REQUIRE(fresh != nullptr);
	BOOST_TEST(contexts().level_set().layer() == fresh);
	BOOST_TEST(!services.descs()->is_registered("editor_game_desc"));
	BOOST_TEST(contexts().level_set().find("w0") != nullptr);
	BOOST_TEST(contexts().level_set().find("w1") != nullptr);
	BOOST_TEST((state.world_names == std::vector<std::string>{ "w0", "w1" }), boost::test_tools::per_element());
	BOOST_TEST(state.active_world_idx == 1);
}

BOOST_AUTO_TEST_CASE(the_editor_rebuilds_its_level_after_the_module_registered)
{
	open_level(put("lv.desc", BASE_LEVEL));
	loader.load(std::make_unique<editor_game_module>(), root);
	BOOST_REQUIRE(module_types_at_attach.size() == 1u); // the game layer changes once
	BOOST_TEST(module_types_at_attach.front());
	loader.unload("editor_game", root);
}

BOOST_AUTO_TEST_CASE(a_component_of_the_module_survives_unload_and_reload_in_the_edited_level)
{
	loader.load(std::make_unique<editor_game_module>(), root);
	open_level(put("lv.desc", MODULE_LEVEL));
	BOOST_REQUIRE(level_has_module_component());

	loader.unload("editor_game", root);
	BOOST_TEST(level_has_module_component()); // kept as written while the module is away
	loader.load(std::make_unique<editor_game_module>(), root);
	BOOST_TEST(level_has_module_component());
	loader.unload("editor_game", root);
}

BOOST_AUTO_TEST_CASE(without_an_open_level_a_layer_change_leaves_the_editor_empty)
{
	loader.load(std::make_unique<editor_game_module>(), root);
	BOOST_TEST(state.world_names.empty());
	BOOST_TEST(state.active_world_idx == -1);
	BOOST_TEST(contexts().level_set().size() == 0u);
	loader.unload("editor_game", root);
	BOOST_TEST(state.world_names.empty());
}

BOOST_AUTO_TEST_SUITE_END()
