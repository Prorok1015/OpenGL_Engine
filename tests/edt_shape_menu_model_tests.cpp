#include <boost/test/unit_test.hpp>
#include "edt_shape_menu_model.h"
#include "edt_shape_request.h"
#include <snk/editor/edt_action_registry.h>
#include <boost/json.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

// #746 S3: the Add shape menu without ImGui: the dialog's fields and what they send to shape.create, the node refs and the commands
// a pick runs in a level.

namespace json = boost::json;
using namespace edt::mesh;

namespace {

const shape_field& field_of(const parametric_shape_form& form, std::string_view key)
{
	for (const shape_field& field : form.fields()) {
		if (field.key == key) return field;
	}
	BOOST_FAIL("no field " << key);
	return form.fields().front();
}

struct recorded_commands
{
	edt::action_registry actions;
	std::vector<std::string> ids;
	std::vector<json::object> arguments;
	std::vector<edt::registration> registrations;

	void stub(const char* id, cmd::result answer)
	{
		cmd::command_desc desc;
		desc.id = id;
		desc.summary = "stub";
		desc.args_schema = json::parse(R"({"type":"object"})");
		desc.result_schema = json::parse(R"({"type":"object"})");
		desc.mutates = true;
		desc.category = cmd::agent_category::scene_edit;
		desc.invoke = [this, id = std::string(id), answer = std::move(answer)](const cmd::call& call) {
			ids.push_back(id);
			arguments.push_back(call.args.as_object());
			return answer;
		};
		registrations.push_back(actions.commands().add(std::move(desc)));
	}
};

}

BOOST_AUTO_TEST_SUITE(EdtShapeMenuModel)

BOOST_AUTO_TEST_CASE(EveryKind_HasItsDefaultFields_AndAFormThatTheCommandTakes)
{
	for (const std::string_view kind : shape_kinds) {
		BOOST_TEST_CONTEXT(kind) {
			parametric_shape_form form;
			form.set_kind(kind);
			form.name = "thing";
			BOOST_TEST(form.kind() == kind);
			BOOST_TEST(!form.fields().empty());
			BOOST_TEST(form.problem() == "");
			BOOST_TEST(form.create_args().at("kind").as_string() == kind);
		}
	}
}

BOOST_AUTO_TEST_CASE(TheFieldsOfASphere_AreNumbers_ThatCountsAreWhole)
{
	parametric_shape_form form;
	form.set_kind("sphere");
	BOOST_TEST(!field_of(form, "radius").integer);
	BOOST_TEST(field_of(form, "segments").integer);
	BOOST_TEST(field_of(form, "rings").integer);
	form.set_kind("box");
	BOOST_TEST(field_of(form, "size").values.size() == 3u);
}

BOOST_AUTO_TEST_CASE(CreateArgs_CarryTheNameKindFolderAndTheEditedParams)
{
	parametric_shape_form form;
	form.set_kind("sphere");
	form.name = "ball";
	form.folder = "props";
	for (shape_field& field : form.fields()) {
		if (field.key == "radius") field.values = { 2.5 };
		if (field.key == "segments") field.values = { 12 };
	}

	const json::object args = form.create_args();

	BOOST_TEST(args.at("name").as_string() == "ball");
	BOOST_TEST(args.at("kind").as_string() == "sphere");
	BOOST_TEST(args.at("folder").as_string() == "props");
	BOOST_TEST(args.at("params").at("radius").to_number<double>() == 2.5);
	BOOST_TEST(args.at("params").at("segments").is_uint64());
	BOOST_TEST(args.at("params").at("segments").to_number<int>() == 12);
}

BOOST_AUTO_TEST_CASE(CreateArgs_WithoutAFolder_LeaveItOut)
{
	parametric_shape_form form;
	form.name = "a";
	BOOST_TEST(!form.create_args().contains("folder"));
}

BOOST_AUTO_TEST_CASE(Problem_NamesAnEmptyName_AndAParameterTheGeneratorRefuses)
{
	parametric_shape_form form;
	BOOST_TEST(form.problem() != "");
	form.name = "a";
	form.set_kind("sphere");
	for (shape_field& field : form.fields()) {
		if (field.key == "radius") field.values = { -1 };
	}
	BOOST_TEST(form.problem() != "");
	for (shape_field& field : form.fields()) {
		if (field.key == "radius") field.values = { 1 };
		if (field.key == "segments") field.values = { 2 };
	}
	BOOST_TEST(form.problem() != "");
}

BOOST_AUTO_TEST_CASE(SetKind_KeepsTheNameAndFolder_AndTakesTheNewKindsDefaults)
{
	parametric_shape_form form;
	form.name = "keep";
	form.folder = "here";
	form.set_kind("capsule");
	BOOST_TEST(form.name == "keep");
	BOOST_TEST(form.folder == "here");
	BOOST_TEST(field_of(form, "hemisphere_rings").integer);
}

BOOST_AUTO_TEST_CASE(NodeRef_SpellsTheIdsAsTheCommandsDo)
{
	const json::object root = node_ref("main", {});
	BOOST_TEST(root.at("context").as_string() == "main");
	BOOST_TEST(root.at("path").as_array().empty());
	const std::vector<std::uint64_t> ids{ 0xa1, 0x2b };
	const json::object ref = node_ref("main", ids);
	BOOST_REQUIRE(ref.at("path").as_array().size() == 2u);
	BOOST_TEST(ref.at("path").as_array()[0].as_string() == "00000000000000a1");
	BOOST_TEST(ref.at("path").as_array()[1].as_string() == "000000000000002b");
}

BOOST_AUTO_TEST_CASE(MeshFieldsOfCreated_NeedsTheWholeRange)
{
	json::object created{ { "geometry", "shapes/a.geom.desc" }, { "vx_begin", 0 }, { "vx_end", 4 }, { "ind_begin", 0 }, { "ind_end", 6 }, { "poly", "x" } };
	const auto fields = mesh_fields_of_created(created);
	BOOST_REQUIRE(fields.has_value());
	BOOST_TEST(fields->size() == 5u);
	BOOST_TEST(!fields->contains("poly"));
	created.erase("ind_end");
	BOOST_TEST(!mesh_fields_of_created(created).has_value());
}

BOOST_AUTO_TEST_CASE(ABaseShapeNode_HoldsTheMeshOfItsFile_WithTheRangeOfTheFile)
{
	const base_shape& shape = base_shapes().front();
	const scn::prefab_desc::prefab_node node = base_shape_node(shape);
	const material_range range = base_shape_range(shape);
	BOOST_TEST(node.name == std::string(shape.name));
	BOOST_TEST(node.node_id != 0u);
	const json::object& fields = node.components.at("mesh").overrides;
	BOOST_TEST(fields.at("geometry").as_string() == base_shape_path(shape));
	BOOST_TEST(fields.at("vx_end").to_number<std::uint64_t>() == range.vx_end);
	BOOST_TEST(fields.at("ind_end").to_number<std::uint64_t>() == range.ind_end);
}

BOOST_AUTO_TEST_CASE(RunShapeRequest_OfABaseShape_CallsAddBaseUnderTheParent)
{
	recorded_commands run;
	run.stub("shape.add_base", cmd::result::success());

	const std::string error = edt::run_shape_request(run.actions, "main", { "box", {}, { 0xa1 } });

	BOOST_TEST(error == "");
	BOOST_REQUIRE(run.ids.size() == 1u);
	BOOST_TEST(run.arguments[0].at("shape").as_string() == "box");
	BOOST_TEST(run.arguments[0].at("parent").at("context").as_string() == "main");
	BOOST_TEST(run.arguments[0].at("parent").at("path").as_array().size() == 1u);
}

BOOST_AUTO_TEST_CASE(RunShapeRequest_OfAParametricShape_CreatesThenAddsTheFileItMade)
{
	recorded_commands run;
	run.stub("node.get", cmd::result::success());
	run.stub("shape.create", cmd::result::success(json::object{ { "poly", "shapes/ball.poly.desc" } }));
	run.stub("shape.add", cmd::result::success());

	const std::string error = edt::run_shape_request(run.actions, "main", { {}, json::object{ { "name", "ball" }, { "kind", "sphere" } }, {} });

	BOOST_TEST(error == "");
	BOOST_REQUIRE(run.ids.size() == 3u);
	BOOST_TEST(run.ids[0] == "node.get");
	BOOST_TEST(run.ids[1] == "shape.create");
	BOOST_TEST(run.ids[2] == "shape.add");
	BOOST_TEST(run.arguments[2].at("poly").as_string() == "shapes/ball.poly.desc");
	BOOST_TEST(run.arguments[2].at("name").as_string() == "ball");
}

BOOST_AUTO_TEST_CASE(RunShapeRequest_StopsAtTheFirstRefusal_AndSaysWhich)
{
	recorded_commands run;
	run.stub("node.get", cmd::result::success());
	run.stub("shape.create", cmd::result::failure(cmd::codes::needs_input, "the file exists"));
	run.stub("shape.add", cmd::result::success());

	const std::string error = edt::run_shape_request(run.actions, "main", { {}, json::object{ { "name", "ball" }, { "kind", "sphere" } }, {} });

	BOOST_TEST(error.find("shape.create") != std::string::npos);
	BOOST_TEST(error.find("the file exists") != std::string::npos);
	BOOST_TEST(run.ids.size() == 2u);

	recorded_commands second;
	second.stub("shape.add_base", cmd::result::failure(cmd::codes::invalid_args, "inside a prefab instance"));
	BOOST_TEST(edt::run_shape_request(second.actions, "main", { "box", {}, {} }).find("inside a prefab instance") != std::string::npos);
	BOOST_TEST(edt::run_shape_request(second.actions, "main", { {}, {}, {} }) != "");
}

BOOST_AUTO_TEST_CASE(RunShapeRequest_UnderANodeThatIsGone_WritesNoFiles)
{
	recorded_commands run;
	run.stub("node.get", cmd::result::failure(cmd::codes::not_found, "no node at that path"));
	run.stub("shape.create", cmd::result::success(json::object{ { "poly", "shapes/ball.poly.desc" } }));
	run.stub("shape.add", cmd::result::success());
	const shape_request request{ {}, json::object{ { "name", "ball" }, { "kind", "sphere" } }, { 0xa1 } };

	const std::string error = edt::run_shape_request(run.actions, "main", request);

	BOOST_TEST(error.find("no node at that path") != std::string::npos);
	BOOST_REQUIRE(run.ids.size() == 1u);
	BOOST_TEST(run.ids[0] == "node.get");
}

BOOST_AUTO_TEST_CASE(RunShapeRequest_WhenTheNodeIsRefusedAfterTheFiles_SaysWhichFilesWereWritten)
{
	recorded_commands run;
	run.stub("node.get", cmd::result::success());
	run.stub("shape.create", cmd::result::success(json::object{ { "poly", "shapes/ball.poly.desc" } }));
	run.stub("shape.add", cmd::result::failure(cmd::codes::busy, "the scene is playing"));

	const std::string error = edt::run_shape_request(run.actions, "main", { {}, json::object{ { "name", "ball" }, { "kind", "sphere" } }, {} });

	BOOST_TEST(error.find("the scene is playing") != std::string::npos);
	BOOST_TEST(error.find("shapes/ball.poly.desc") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Problem_RefusesCountsThatAreNotWholeNumbersOfAnUnsignedInt_AndNumbersThatAreNotFinite)
{
	for (const double bad : { std::nan(""), std::numeric_limits<double>::infinity(), 1e30, -3.0, 3.5 }) {
		BOOST_TEST_CONTEXT(bad) {
			parametric_shape_form form;
			form.name = "a";
			form.set_kind("sphere");
			for (shape_field& field : form.fields()) {
				if (field.key == "segments") field.values = { bad };
			}
			BOOST_TEST(form.problem() != "");
		}
	}
	parametric_shape_form real;
	real.name = "a";
	real.set_kind("sphere");
	for (shape_field& field : real.fields()) {
		if (field.key == "radius") field.values = { std::numeric_limits<double>::quiet_NaN() };
	}
	BOOST_TEST(real.problem() != "");
}

BOOST_AUTO_TEST_SUITE_END()
