#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/scn_mesh_node_desc.h>
#include <snk/scene/scn_model.h>
#include <boost/json.hpp>
#include <entt/entt.hpp>

// #560: a mesh is drawn while it has scn::renderable; mesh_node_desc says "visible": false only when it is hidden.

namespace {

boost::json::object without_internal_keys(boost::json::object data)
{
	for (auto it = data.begin(); it != data.end();) {
		it = it->key().starts_with("__") ? data.erase(it) : std::next(it);
	}
	return data;
}

}

BOOST_FIXTURE_TEST_SUITE(ScnMeshVisible, test_util::scene_env)

BOOST_AUTO_TEST_CASE(SetVisible_TakesAndGivesBackTheRenderableMark)
{
	entt::registry reg;
	const auto e = reg.create();
	BOOST_TEST(!scn::is_visible(reg, e));
	scn::set_visible(reg, e, true);
	BOOST_TEST(scn::is_visible(reg, e));
	BOOST_TEST(reg.all_of<scn::renderable>(e));
	scn::set_visible(reg, e, true);
	BOOST_TEST(scn::is_visible(reg, e));
	scn::set_visible(reg, e, false);
	BOOST_TEST(!scn::is_visible(reg, e));
	scn::set_visible(reg, e, false);
	BOOST_TEST(!reg.all_of<scn::renderable>(e));
}

BOOST_AUTO_TEST_CASE(AHiddenMeshNode_IsAssembledWithoutTheMark_AndAgainWithItWhenShown)
{
	entt::registry reg;
	const auto e = reg.create();
	scn::mesh_node_desc desc;
	desc.vx_end = 24;
	desc.ind_end = 36;

	desc.visible = false;
	scn::assemble_mesh_node(reg, e, desc, "mesh");
	BOOST_TEST(reg.all_of<scn::mesh_component>(e));
	BOOST_TEST(!scn::is_visible(reg, e));

	// A hot reload assembles over the same entity.
	desc.visible = true;
	scn::assemble_mesh_node(reg, e, desc, "mesh");
	BOOST_TEST(scn::is_visible(reg, e));
	desc.visible = false;
	scn::assemble_mesh_node(reg, e, desc, "mesh");
	BOOST_TEST(!scn::is_visible(reg, e));
}

BOOST_AUTO_TEST_CASE(AMeshFileWithoutTheField_IsVisible_AndIsWrittenBackUnchanged)
{
	const std::string text = R"({"__type":"mesh_node_desc","vx_begin":0,"vx_end":24,"ind_begin":0,"ind_end":36})";
	const auto mesh = require_loaded<scn::mesh_node_desc>(put("imported_mesh.desc", text));
	BOOST_REQUIRE(mesh.is_ready());
	BOOST_TEST(mesh->visible);

	boost::json::object written;
	mesh->serialize(written);
	BOOST_TEST(!written.contains("visible"));
	BOOST_TEST((without_internal_keys(written) == without_internal_keys(boost::json::parse(text).as_object())));
}

BOOST_AUTO_TEST_CASE(AHiddenMesh_KeepsVisibleFalseThroughAFile)
{
	const auto mesh = require_loaded<scn::mesh_node_desc>(
		put("hidden_mesh.desc", R"({"__type":"mesh_node_desc","vx_end":24,"ind_end":36,"visible":false})"));
	BOOST_REQUIRE(mesh.is_ready());
	BOOST_TEST(!mesh->visible);

	boost::json::object written;
	mesh->serialize(written);
	BOOST_REQUIRE(written.contains("visible"));
	BOOST_TEST(written.at("visible").as_bool() == false);

	written["__type"] = "mesh_node_desc";
	const auto again = require_loaded<scn::mesh_node_desc>(put("hidden_mesh_again.desc", boost::json::serialize(written)));
	BOOST_REQUIRE(again.is_ready());
	BOOST_TEST(!again->visible);
}

BOOST_AUTO_TEST_SUITE_END()
