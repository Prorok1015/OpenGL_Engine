#include <boost/test/unit_test.hpp>
#include "edt_code_commands.h"
#include "edt_mesh_io.h"
#include "edt_shape_menu_model.h"
#include "edt_shape_parameters.h"
#include "edt_mesh_shape_asset.h"
#include <boost/json.hpp>
#include <cstddef>
#include <string>

// #746 S5: what the Inspector's "Shape parameters" section knows of a shape, and what a person is told before Rebuild.

namespace json = boost::json;
using namespace edt::mesh;

namespace {

edt::desc_file_read file_of(const std::string& text, bool top_layer = true)
{
	edt::desc_file_read out;
	out.located = true;
	out.top_layer = top_layer;
	for (const char c : text) out.bytes.push_back(static_cast<std::byte>(c));
	out.version = edt::code_version_of(out.bytes);
	return out;
}

std::string poly_of(const shape_params& params)
{
	const shape_asset_result made = build_shape_asset(params, res::tag::make("shapes/probe.geom.desc"));
	BOOST_REQUIRE_MESSAGE(made.files.has_value(), made.error);
	return made.files->poly;
}

scn::prefab_desc::prefab_node mesh_node_of(const std::string& geometry, int vx_end)
{
	return mesh_node("n", mesh_fields(geometry, material_range{ 0, 0, static_cast<std::uint32_t>(vx_end), 0, 6 }));
}

}

BOOST_AUTO_TEST_SUITE(EdtShapeParameters)

BOOST_AUTO_TEST_CASE(ThePolyPath_IsTheGeometryPathWithItsSuffixChanged)
{
	BOOST_TEST(poly_path_of_geometry("res://shapes/ball.geom.desc") == "shapes/ball.poly.desc");
	BOOST_TEST(poly_path_of_geometry("shapes/ball.geom.desc") == "shapes/ball.poly.desc");
	BOOST_TEST(poly_path_of_geometry("res://a/b/c.geom.desc") == "a/b/c.poly.desc");
	BOOST_TEST(poly_path_of_geometry("res://shapes/ball.desc") == "");
	BOOST_TEST(poly_path_of_geometry("res://.geom.desc") == "");
	BOOST_TEST(poly_path_of_geometry("") == "");
}

BOOST_AUTO_TEST_CASE(AShapeTheEditorMade_IsEditable_WithItsStoredKindParamsAndVersion)
{
	sphere_params sphere;
	sphere.radius = 2.5f;
	sphere.segments = 12;
	const edt::desc_file_read file = file_of(poly_of(sphere));

	const shape_parameters shape = load_shape_parameters("shapes/ball.poly.desc", file);

	BOOST_REQUIRE(shape.what == shape_parameters::state::editable);
	BOOST_TEST(shape.kind == "sphere");
	BOOST_TEST(shape.poly == "shapes/ball.poly.desc");
	BOOST_TEST(shape.version == file.version);
	BOOST_TEST(shape.params.at("radius").to_number<double>() == 2.5);
	BOOST_TEST(shape.params.at("segments").to_number<int>() == 12);
}

BOOST_AUTO_TEST_CASE(TheFormTakesTheStoredParameters_AndGivesThemBackToShapeRebuild)
{
	capsule_params capsule;
	capsule.radius = 0.75f;
	capsule.height = 3.f;
	const shape_parameters shape = load_shape_parameters("shapes/c.poly.desc", file_of(poly_of(capsule)));
	BOOST_REQUIRE(shape.what == shape_parameters::state::editable);

	parametric_shape_form form;
	std::string error;
	BOOST_REQUIRE_MESSAGE(form.load(shape.kind, shape.params, error), error);
	BOOST_TEST(form.kind() == "capsule");
	BOOST_TEST(form.parameters_problem() == "");
	for (shape_field& field : form.fields()) {
		if (field.key == "radius") field.values = { 1.0 };
	}

	const json::object args = rebuild_arguments(shape, form.params());
	BOOST_TEST(args.at("path").as_string() == "shapes/c.poly.desc");
	BOOST_TEST(args.at("version").as_string() == shape.version);
	BOOST_TEST(args.at("kind").as_string() == "capsule");
	BOOST_TEST(args.at("params").at("radius").to_number<double>() == 1.0);
	BOOST_TEST(args.at("params").at("height").to_number<double>() == 3.0);
}

BOOST_AUTO_TEST_CASE(AFormWithABadParameter_NamesIt_WithoutANameBeingNeeded)
{
	parametric_shape_form form;
	form.set_kind("sphere");
	for (shape_field& field : form.fields()) {
		if (field.key == "segments") field.values = { 2 };
	}
	BOOST_TEST(form.parameters_problem() != "");
	BOOST_TEST(form.name.empty());
}

BOOST_AUTO_TEST_CASE(AMeshEditedByHand_AFileOfALowerLayer_AndAFileThatIsNotThere_AreNotEditable)
{
	std::string hand_made = poly_of(box_params{});
	json::object parsed = json::parse(hand_made).as_object();
	parsed.erase("generator");
	const shape_parameters edited = load_shape_parameters("shapes/a.poly.desc", file_of(json::serialize(parsed)));
	BOOST_TEST((edited.what == shape_parameters::state::edited_by_hand));
	BOOST_TEST(edited.reason != "");

	const shape_parameters lower = load_shape_parameters("shapes/a.poly.desc", file_of(poly_of(box_params{}), false));
	BOOST_TEST((lower.what == shape_parameters::state::lower_layer));

	const shape_parameters missing = load_shape_parameters("shapes/a.poly.desc", edt::desc_file_read{});
	BOOST_TEST((missing.what == shape_parameters::state::not_a_shape));

	edt::desc_file_read broken = file_of("");
	broken.error = "the file cannot be read";
	const shape_parameters unreadable = load_shape_parameters("shapes/a.poly.desc", broken);
	BOOST_TEST((unreadable.what == shape_parameters::state::unreadable));
	BOOST_TEST(unreadable.reason == "the file cannot be read");
}

BOOST_AUTO_TEST_CASE(OnlyTheNodesWhoseRangeChanges_AreCounted_AndRetargetingChangesExactlyThose)
{
	const std::string wanted = std::string("res://") + "shapes/x.geom.desc";
	scn::prefab_desc::prefab_node root;
	root.children.push_back(mesh_node_of(wanted, 4));
	root.children.push_back(mesh_node_of("shapes/x.geom.desc", 50));
	root.children.push_back(mesh_node_of("shapes/other.geom.desc", 4));
	root.children[0].children.push_back(mesh_node_of(wanted, 4));

	json::object range{ { "vx_begin", 0 }, { "vx_end", 50 }, { "ind_begin", 0 }, { "ind_end", 6 } };

	const std::size_t stale = count_stale_shape_nodes(root, wanted, range);
	BOOST_TEST(stale == 2u);
	BOOST_TEST(count_stale_shape_nodes(root, "shapes/x.geom.desc", range) == 2u);
	BOOST_TEST(retarget_shape_nodes(root, wanted, range) == stale);
	BOOST_TEST(root.children[0].components.at("mesh").overrides.at("vx_end").to_number<int>() == 50);
	BOOST_TEST(root.children[0].children[0].components.at("mesh").overrides.at("vx_end").to_number<int>() == 50);
	BOOST_TEST(root.children[2].components.at("mesh").overrides.at("vx_end").to_number<int>() == 4);
	BOOST_TEST(count_stale_shape_nodes(root, wanted, range) == 0u);
	BOOST_TEST(retarget_shape_nodes(root, wanted, range) == 0u);
}

BOOST_AUTO_TEST_CASE(AFieldANodeDoesNotHave_IsZero_SoANodeAlreadyAtTheRangeIsNotChanged)
{
	scn::prefab_desc::prefab_node root = mesh_node("n", json::object{ { "geometry", "shapes/x.geom.desc" }, { "vx_end", 4 }, { "ind_end", 6 } });
	json::object range{ { "vx_begin", 0 }, { "vx_end", 4 }, { "ind_begin", 0 }, { "ind_end", 6 } };

	BOOST_TEST(count_stale_shape_nodes(root, "shapes/x.geom.desc", range) == 0u);
	BOOST_TEST(retarget_shape_nodes(root, "shapes/x.geom.desc", range) == 0u);
	BOOST_TEST(!root.components.at("mesh").overrides.contains("vx_begin"));

	range["vx_begin"] = 2;
	BOOST_TEST(count_stale_shape_nodes(root, "shapes/x.geom.desc", range) == 1u);
}

BOOST_AUTO_TEST_CASE(ThePredictedRange_IsTheOneTheRebuiltFileHas_AndABadParameterBuildsNothing)
{
	std::string error;
	const std::optional<json::object> range = predicted_range("box", shape_params_to_json(box_params{}), error);
	BOOST_REQUIRE_MESSAGE(range.has_value(), error);
	BOOST_TEST(range->at("vx_begin").to_number<int>() == 0);
	BOOST_TEST(range->at("vx_end").to_number<int>() > 0);
	BOOST_TEST(range->at("ind_end").to_number<int>() > 0);

	sphere_params sphere;
	sphere.segments = 2;
	BOOST_TEST(!predicted_range("sphere", shape_params_to_json(sphere), error).has_value());
	BOOST_TEST(error != "");
}

BOOST_AUTO_TEST_CASE(TheRangeOfAnAnswer_NeedsAllFourFields)
{
	json::object answer{ { "vx_begin", 0 }, { "vx_end", 4 }, { "ind_begin", 0 }, { "ind_end", 6 }, { "poly", "x" } };
	const auto range = range_of_rebuilt(answer);
	BOOST_REQUIRE(range.has_value());
	BOOST_TEST(range->size() == 4u);
	answer.erase("ind_end");
	BOOST_TEST(!range_of_rebuilt(answer).has_value());
}

BOOST_AUTO_TEST_CASE(TheConfirmation_SaysItCannotBeUndone_HowManyNodesChange_AndWhatStaysUnsaved)
{
	rebuild_facts facts;
	facts.poly = "shapes/ball.poly.desc";
	facts.level_open = true;
	facts.level_nodes = 3;
	facts.level_dirty = true;
	facts.level_agent_changes = true;
	facts.prefab_open = true;
	facts.prefab_nodes = 2;
	facts.prefab_dirty = true;

	const std::string text = rebuild_confirmation(facts);

	BOOST_TEST(text.find("shapes/ball.poly.desc") != std::string::npos);
	BOOST_TEST(text.find("Rebuild cannot be undone; the level's undo history will be cleared") != std::string::npos);
	BOOST_TEST(text.find("3 mesh node(s) of the open level") != std::string::npos);
	BOOST_TEST(text.find("2 mesh node(s) of the open prefab") != std::string::npos);
	BOOST_TEST(text.find("not open keep the old range") != std::string::npos);
	BOOST_TEST(text.find("unsaved changes; they stay unsaved") != std::string::npos);
	BOOST_TEST(text.find("working copy on top of them") != std::string::npos);
	BOOST_TEST(text.find("Agent edits nobody accepted") != std::string::npos);

	facts.level_dirty = false;
	facts.level_agent_changes = false;
	facts.prefab_dirty = false;
	const std::string clean = rebuild_confirmation(facts);
	BOOST_TEST(clean.find("after the rebuild it has") != std::string::npos);
	BOOST_TEST(clean.find("Agent edits") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheConfirmation_WithNoNodeChanging_KeepsTheHistory_AndSaysNothingIsUnsaved)
{
	rebuild_facts facts;
	facts.poly = "shapes/ball.poly.desc";
	facts.level_open = true;
	facts.level_dirty = true;
	facts.prefab_open = true;
	facts.prefab_dirty = true;

	const std::string text = rebuild_confirmation(facts);

	BOOST_TEST(text.find("No mesh node of the open level needs a new range") != std::string::npos);
	BOOST_TEST(text.find("No mesh node of the open prefab needs a new range") != std::string::npos);
	BOOST_TEST(text.find("undo history will be cleared") == std::string::npos);
	BOOST_TEST(text.find("the undo history is kept") != std::string::npos);
	BOOST_TEST(text.find("unsaved") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheConfirmation_NamesWhyAnOpenPrefabIsLeftAsItIs)
{
	rebuild_facts facts;
	facts.poly = "shapes/ball.poly.desc";
	facts.prefab_open = true;
	facts.prefab_nodes = 2;
	facts.prefab_dirty = true;
	facts.prefab_blocker = "this prefab belongs to a lower layer";

	const std::string text = rebuild_confirmation(facts);

	BOOST_TEST(text.find("The open prefab keeps the old range of its nodes: this prefab belongs to a lower layer") != std::string::npos);
	BOOST_TEST(text.find("working copy") == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
