#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_component_renderers.h"
#include "edt_schema_inspector.h"
#include <snk/desc/desc_json_ops.h>
#include <snk/editor/edt_inspector_registry.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_mesh_node_desc.h>
#include <boost/json.hpp>
#include <functional>
#include <string>
#include <string_view>

// The Inspector draws mesh_node_desc from its schema (#720; #560 had a renderer of its own with a Visible box). These
// tests run the edit the Inspector makes of a component (schema_ui::edit_component, which render_schema_component calls
// with the ImGui widgets): the base is the parent's data, the edit is written back as the pointers it changed against it.

namespace {

namespace json = boost::json;

const json::object& parent_fields()
{
	static const json::object fields{ { "vx_begin", 0 }, { "vx_end", 24 }, { "ind_begin", 0 }, { "ind_end", 36 } };
	return fields;
}

scn::prefab_desc::prefab_comp_node mesh_component()
{
	scn::prefab_desc::prefab_comp_node comp;
	comp.type_name = std::string(scn::mesh_node_desc::__type);
	return comp;
}

struct mesh_inspector_fixture : test_util::scene_env
{
	const desc::type_schema& schema() const
	{
		const desc::type_schema* found = descs->find_schema(std::string(scn::mesh_node_desc::__type));
		BOOST_REQUIRE(found);
		return *found;
	}

	// The values the Inspector shows for the node.
	json::value shown(const scn::prefab_desc::prefab_comp_node& node, const json::object* parent) const
	{
		return edt::schema_ui::compose(schema(), descs->schemas(), { parent, &node.overrides });
	}

	// One edit in the Inspector, through what inspector_panel::render_schema_component runs: `change` stands in for the
	// widgets, the difference lands in the node's own data.
	bool edit(scn::prefab_desc::prefab_comp_node& node, const json::object* parent, const std::function<void(json::value&)>& change) const
	{
		return edt::schema_ui::edit_component(schema(), descs->schemas(), parent, node.overrides, [&change](json::value& shown) {
			change(shown);
			return true;
		});
	}

	static bool visible_in(const json::value& shown)
	{
		const json::value* value = desc::json_pointer_get(shown, "/visible");
		return value && value->is_bool() && value->as_bool();
	}
};

void set_visible(json::value& shown, bool visible)
{
	desc::json_pointer_set(shown, "/visible", json::value(visible));
}

}

BOOST_FIXTURE_TEST_SUITE(EdtMeshVisibleInspector, mesh_inspector_fixture)

BOOST_AUTO_TEST_CASE(TheMeshNode_IsDrawnFromItsSchema_NotByARendererOfItsOwn)
{
	edt::inspector_registry registry;
	const auto registrations = edt::register_desc_component_renderers(registry);
	BOOST_TEST(!registry.contains(std::string(scn::mesh_node_desc::__type)));

	const desc::type_schema& mesh = schema();
	const auto field = [&mesh](std::string_view key) {
		const desc::field_schema* found = mesh.find_field(key);
		return found ? found : mesh.find_field("/" + std::string(key));
	};
	for (const char* key : { "geometry", "vx_begin", "vx_end", "ind_begin", "ind_end", "material", "visible" }) {
		BOOST_TEST_CONTEXT(key) BOOST_TEST(field(key) != nullptr);
	}
	BOOST_REQUIRE(field("geometry") && field("material") && field("visible"));
	BOOST_TEST((field("geometry")->kind == desc::field_kind::desc_ref));
	BOOST_TEST((field("material")->kind == desc::field_kind::desc_ref));
	BOOST_TEST((field("visible")->kind == desc::field_kind::boolean));
}

BOOST_AUTO_TEST_CASE(HidingAMeshOfItsParent_KeepsOnlyVisibleFalse)
{
	auto node = mesh_component();
	BOOST_TEST(visible_in(shown(node, &parent_fields())));
	BOOST_TEST(edit(node, &parent_fields(), [](json::value& shown) { set_visible(shown, false); }));
	BOOST_TEST((node.overrides == json::object{ { "visible", false } }));
	BOOST_TEST(!visible_in(shown(node, &parent_fields())));

	BOOST_TEST(edit(node, &parent_fields(), [](json::value& shown) { set_visible(shown, true); }));
	BOOST_TEST(visible_in(shown(node, &parent_fields())));
}

// The base hides the mesh: the parent's data says false. Showing it must leave a value of the node's own, not drop the key.
BOOST_AUTO_TEST_CASE(ShowingAMeshItsBaseHides_LeavesVisibleTrue)
{
	json::object base = parent_fields();
	base["visible"] = false;
	auto node = mesh_component();
	BOOST_TEST(!visible_in(shown(node, &base)));
	BOOST_TEST(edit(node, &base, [](json::value& shown) { set_visible(shown, true); }));
	BOOST_TEST((node.overrides == json::object{ { "visible", true } }));
	BOOST_TEST(visible_in(shown(node, &base)));

	// Back to what the base says: the node's own value goes and the base hides it again.
	BOOST_TEST(edit(node, &base, [](json::value& shown) { set_visible(shown, false); }));
	BOOST_TEST(node.overrides.empty());
	BOOST_TEST(!visible_in(shown(node, &base)));
}

BOOST_AUTO_TEST_CASE(AnUntouchedComponent_ChangesNothing)
{
	auto node = mesh_component();
	BOOST_TEST(!edit(node, &parent_fields(), [](json::value&) {}));
	BOOST_TEST(node.overrides.empty());
}

// The fields #721 left read-only are edited again: the material and the range of a node.
BOOST_AUTO_TEST_CASE(TheMaterialAndTheRange_AreEditedThroughTheSchema)
{
	auto node = mesh_component();
	BOOST_TEST(edit(node, &parent_fields(), [](json::value& shown) {
		desc::json_pointer_set(shown, "/material", json::value("res://materials/red.mat.desc"));
		desc::json_pointer_set(shown, "/ind_end", json::value(30));
	}));
	BOOST_TEST((node.overrides == json::object{ { "material", "res://materials/red.mat.desc" }, { "ind_end", 30 } }));
}

BOOST_AUTO_TEST_SUITE_END()
