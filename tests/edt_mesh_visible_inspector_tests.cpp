#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_component_renderers.h"
#include "edt_cr_internal.h"
#include "edt_inspector_panel.h"
#include <snk/editor/edt_inspector_registry.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_mesh_node_desc.h>
#include <boost/json.hpp>

// #560: the Visible box of mesh_node_desc in the inspector. The inspector shows a node without its own data on a copy
// of its parent's and keeps only what the renderer changed against the parent (edit_against_parent), so ticking the
// box off on a mesh of an imported model gives the node {"visible": false} and nothing else.

namespace {

const boost::json::object& parent_fields()
{
	static const boost::json::object fields{ { "vx_begin", 0 }, { "vx_end", 24 }, { "ind_begin", 0 }, { "ind_end", 36 } };
	return fields;
}

scn::prefab_desc::prefab_comp_node mesh_component()
{
	scn::prefab_desc::prefab_comp_node comp;
	comp.type_name = std::string(scn::mesh_node_desc::__type);
	return comp;
}

}

BOOST_FIXTURE_TEST_SUITE(EdtMeshVisibleInspector, test_util::scene_env)

BOOST_AUTO_TEST_CASE(TheMeshRenderer_IsRegisteredWithTheBuiltIns)
{
	edt::inspector_registry registry;
	const auto registrations = edt::register_desc_component_renderers(registry);
	BOOST_TEST(registry.contains(std::string(scn::mesh_node_desc::__type)));
}

BOOST_AUTO_TEST_CASE(HidingAMeshOfItsParent_KeepsOnlyVisibleFalse_ThroughTheInspectorsDiff)
{
	auto node = mesh_component();
	const bool changed = edt::edit_against_parent(node, &parent_fields(), [](scn::prefab_desc::prefab_comp_node& scratch) {
		BOOST_TEST(edt::mesh_node_visible(scratch));
		edt::set_mesh_node_visible(scratch, false);
		return true;
	});
	BOOST_TEST(changed);
	BOOST_TEST((node.overrides == boost::json::object{ { "visible", false } }));
	BOOST_TEST(!edt::mesh_node_visible(node));

	// With data of its own the renderer edits the node itself.
	edt::set_mesh_node_visible(node, true);
	BOOST_TEST(edt::mesh_node_visible(node));
	BOOST_TEST((node.overrides == boost::json::object{ { "visible", true } }));
}

// The base hides the mesh by an override of its own node: the parent's data the inspector passes says false while the
// desc it refers to is visible. Showing it must leave a record, not drop the key.
BOOST_AUTO_TEST_CASE(ShowingAMeshItsBaseHides_LeavesVisibleTrue_ThroughTheInspectorsDiff)
{
	boost::json::object base = parent_fields();
	base["visible"] = false;
	auto node = mesh_component();
	const bool changed = edt::edit_against_parent(node, &base, [](scn::prefab_desc::prefab_comp_node& scratch) {
		BOOST_TEST(!edt::mesh_node_visible(scratch));
		edt::set_mesh_node_visible(scratch, true);
		return true;
	});
	BOOST_TEST(changed);
	BOOST_TEST((node.overrides == boost::json::object{ { "visible", true } }));
}

BOOST_AUTO_TEST_CASE(AnUntouchedBox_ChangesNothing_AndAReadOnlyInspectorKeepsNothing)
{
	auto node = mesh_component();
	BOOST_TEST(!edt::edit_against_parent(node, &parent_fields(), [](scn::prefab_desc::prefab_comp_node&) { return false; }));
	BOOST_TEST(node.overrides.empty());

	// Showing a mesh its visible parent already shows leaves no record.
	BOOST_TEST(edt::edit_against_parent(node, &parent_fields(), [](scn::prefab_desc::prefab_comp_node& scratch) {
		edt::set_mesh_node_visible(scratch, true);
		return true;
	}));
	BOOST_TEST(node.overrides.empty());

	BOOST_TEST(!edt::edit_against_parent(node, &parent_fields(), [](scn::prefab_desc::prefab_comp_node& scratch) {
		edt::set_mesh_node_visible(scratch, false);
		return true;
	}, false));
	BOOST_TEST(node.overrides.empty());
}

BOOST_AUTO_TEST_CASE(AMeshWhoseParentIsHidden_IsShownByAnOverrideOfItsOwn)
{
	auto node = mesh_component();
	node.parent_desc = require_loaded<desc::desc_base>(
		put("hidden_parent_mesh.desc", R"({"__type":"mesh_node_desc","vx_end":24,"ind_end":36,"visible":false})"));
	BOOST_REQUIRE(node.parent_desc.is_ready());
	BOOST_TEST(!edt::mesh_node_visible(node));

	edt::set_mesh_node_visible(node, true);
	BOOST_REQUIRE(node.overrides.contains("visible"));
	BOOST_TEST(node.overrides.at("visible").as_bool() == true);
	BOOST_TEST(edt::mesh_node_visible(node));

	edt::set_mesh_node_visible(node, false);
	BOOST_TEST(!edt::mesh_node_visible(node));
}

BOOST_AUTO_TEST_SUITE_END()
