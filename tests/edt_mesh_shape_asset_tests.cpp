#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_code_commands.h"
#include "edt_mesh_base_shapes.h"
#include "edt_mesh_io.h"
#include "edt_mesh_shape_asset.h"
#include "edt_mesh_shape_commands.h"
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/desc/desc_system.h>
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

// Parametric shapes (#746): the two files of a shape and the commands that write them.
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;
using namespace edt::mesh;

std::string read_file(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

void write_file(const fs::path& path, const std::string& text)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << text;
}

// The desc the writer puts on disk is compact JSON and the one build_shape_asset returns is the repository's pretty form: the same data.
bool same_json(const std::string& left, const std::string& right)
{
	return json::parse(left) == json::parse(right);
}

std::string version_of_text(const std::string& text)
{
	const auto* first = reinterpret_cast<const std::byte*>(text.data());
	return edt::code_version_of(std::vector<std::byte>(first, first + text.size()));
}

shape_asset_files built(const shape_params& params)
{
	const shape_asset_result result = build_shape_asset(params, res::tag{ "res://shapes/ball.geom.desc" });
	BOOST_REQUIRE_MESSAGE(result.files.has_value(), result.error);
	return *result.files;
}

const sphere_params small_sphere{ 0.5f, 8, 4 };

struct shape_fixture : test_util::scene_env
{
	shape_fixture()
	{
		fs::create_directories(lower);
		fs::create_directories(res_root);
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		seen = events.agent_action.subscribe([this](const edt::agent_action_info& info) { actions.push_back(info); });
		edt::desc_file_layout layout;
		layout.top_layer = [this] { return res_root; };
		layout.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		layout.write_blocker = [this] { return blocked; };
		registrations = register_shape_commands(commands, shape_command_deps{ *rs, *descs, std::move(layout), commands, &events });
		stub("scene.node.create", created);
		stub("shape.update_nodes", updated_with, &updated_nodes);
	}

	// A scene command that only records its arguments: shape.add and shape.rebuild reach the scene through them.
	void stub(const char* id, json::object& arguments, const std::size_t* updated = nullptr)
	{
		cmd::command_desc desc;
		desc.id = id;
		desc.summary = "stub";
		desc.args_schema = json::parse(R"({"type":"object"})");
		desc.result_schema = json::parse(R"({"type":"object"})");
		desc.mutates = true;
		desc.category = cmd::agent_category::scene_edit;
		desc.invoke = [&arguments, updated](const cmd::call& call) {
			arguments = call.args.as_object();
			return cmd::result::success(json::object{ { "updated", updated ? *updated : 0u } });
		};
		edt::registration added = commands.add(std::move(desc));
		registrations.push_back(std::move(added));
	}

	~shape_fixture()
	{
		registrations.clear();
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	cmd::result call(const std::string& id, json::value args)
	{
		return commands.invoke(id, std::move(args), cmd::origin::agent("s1"));
	}

	cmd::result create_sphere(const char* name = "ball")
	{
		return call("shape.create", json::object{ { "name", name }, { "kind", "sphere" },
			{ "params", json::object{ { "radius", 0.5 }, { "segments", 8 }, { "rings", 4 } } } });
	}

	json::object rebuild_args(const std::string& path, const std::string& version) const
	{
		return json::object{ { "path", path }, { "version", version } };
	}

	test_util::temp_dir dir{ "edt_mesh_shape_asset" };
	const fs::path res_root = dir.path() / "res";
	const fs::path lower = dir.path() / "lower";
	edt::editor_events events;
	std::vector<edt::agent_action_info> actions;
	edt::connection seen;
	cmd::command_registry commands;
	std::vector<edt::registration> registrations;
	std::optional<std::string> blocked;
	json::object created;
	json::object updated_with;
	std::size_t updated_nodes = 0;
};

}

BOOST_AUTO_TEST_SUITE(EdtMeshShapeAsset)

BOOST_AUTO_TEST_CASE(TheFiles_AreTheSameForTheSameParameters_AndTheBoxIsTheBaseShapesFile)
{
	const shape_asset_files first = built(box_params{});
	const shape_asset_files second = built(box_params{});
	BOOST_TEST(first.poly == second.poly);
	BOOST_TEST(first.geometry == second.geometry);
	const base_shape_pair box = base_shape_files(*find_base_shape("box"));
	BOOST_TEST(first.geometry == box.desc);
	BOOST_TEST((first.binary == box.binary));
	BOOST_TEST(first.vertices == 24u);
	BOOST_TEST(first.indices == 36u);
	BOOST_TEST(first.poly.find("\"__version\"") != std::string::npos);
	BOOST_TEST(first.poly.back() == '\n');
	BOOST_TEST(poly_file_version == 1);
}

BOOST_AUTO_TEST_CASE(TheVertexEstimate_IsTheRealCountOfTheBase_AndNeverWrapsAround)
{
	for (const base_shape& shape : base_shapes()) {
		BOOST_TEST_CONTEXT(shape.name) {
			BOOST_TEST(estimate_shape_vertices(shape.params) == built(shape.params).vertices);
		}
	}
	BOOST_TEST(estimate_shape_vertices(sphere_params{ 0.5f, 0, 0 }) == 0u);
	BOOST_TEST(estimate_shape_vertices(capsule_params{ 0.5f, 1.f, 0, 0 }) == 0u);
}

BOOST_AUTO_TEST_CASE(TheSmallestShapes_AreWholeMeshes)
{
	for (const shape_params& params : { shape_params{ sphere_params{ 0.5f, 3, 2 } }, shape_params{ cylinder_params{ 0.5f, 1.f, 3 } },
			shape_params{ capsule_params{ 0.5f, 1.f, 3, 1 } }, shape_params{ plane_params{ { 1.f, 1.f }, 1, 1 } } }) {
		BOOST_TEST_CONTEXT(shape_kind_name(params)) {
			const shape_asset_files files = built(params);
			BOOST_TEST(files.vertices > 0u);
			BOOST_TEST(files.indices > 0u);
			BOOST_TEST(files.indices % 3 == 0u);
			std::string error;
			const std::optional<material_range> range = poly_text_range(files.poly, error);
			BOOST_REQUIRE_MESSAGE(range.has_value(), error);
			BOOST_TEST(range->vx_end == files.vertices);
			BOOST_TEST(range->ind_end == files.indices);
		}
	}
}

BOOST_AUTO_TEST_CASE(APolyFileThatIsNoMesh_HasNoRange_AndAScaleNeedsPositiveComponents)
{
	std::string error;
	BOOST_TEST(!poly_text_range("[]", error).has_value());
	BOOST_TEST(!poly_text_range("{", error).has_value());
	BOOST_TEST(!poly_text_range(R"({"__type":"poly_mesh_desc","vertices":5})", error).has_value());
	BOOST_TEST(!error.empty());
	BOOST_TEST(shape_scale_problem({ 1.f, 2.f, 3.f }).empty());
	BOOST_TEST(!shape_scale_problem({ 1.f, 0.f, 3.f }).empty());
	BOOST_TEST(!shape_scale_problem({ -1.f, 1.f, 1.f }).empty());
	BOOST_TEST(!shape_scale_problem({ 1.f, 1.f, std::numeric_limits<float>::infinity() }).empty());
}

BOOST_AUTO_TEST_CASE(AShapeThatTheGeneratorRefuses_HasNoFiles)
{
	const shape_asset_result refused = build_shape_asset(sphere_params{ 0.5f, 1000, 4 }, res::tag{ "res://shapes/a.geom.desc" });
	BOOST_TEST(!refused.files.has_value());
	BOOST_TEST(!refused.error.empty());
}

BOOST_AUTO_TEST_CASE(ThePolyFile_KeepsItsGenerator_AndIsGeneratedUntilItIsTouched)
{
	const shape_asset_files files = built(small_sphere);
	const shape_source_check checked = check_shape_source(files.poly);
	BOOST_TEST((checked.state == shape_source::generated));
	BOOST_REQUIRE(checked.generator.has_value());
	BOOST_TEST(shape_kind_name(*checked.generator) == "sphere");
	BOOST_TEST(std::get<sphere_params>(*checked.generator).segments == 8u);
}

BOOST_AUTO_TEST_CASE(AMeshMovedByHand_OrWithoutAGenerator_IsEdited_AndGarbageIsUnreadable)
{
	const shape_asset_files files = built(small_sphere);

	const json::value untouched = json::parse(files.poly);
	BOOST_TEST((check_shape_source(json::serialize(untouched)).state == shape_source::generated));

	json::value moved = untouched;
	moved.as_object().at("vertices").as_array()[0].as_array()[1] = 0.25;
	const shape_source_check moved_check = check_shape_source(json::serialize(moved));
	BOOST_TEST((moved_check.state == shape_source::edited));
	BOOST_TEST(!moved_check.error.empty());

	json::value bare = json::parse(files.poly);
	bare.as_object().erase("generator");
	BOOST_TEST((check_shape_source(json::serialize(bare)).state == shape_source::edited));

	BOOST_TEST((check_shape_source("[1, 2").state == shape_source::unreadable));
	BOOST_TEST((check_shape_source("[]").state == shape_source::unreadable));
	BOOST_TEST((check_shape_source(R"({"__type":"poly_mesh_desc","vertices":5})").state == shape_source::unreadable));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtMeshShapeCommands, shape_fixture)

BOOST_AUTO_TEST_CASE(TheCommands_HaveTheirCategoriesAndOnlyARebuildAsksAPerson)
{
	BOOST_TEST((commands.find("shape.create")->category == cmd::agent_category::scene_edit));
	BOOST_TEST((commands.find("shape.rebuild")->category == cmd::agent_category::scene_edit));
	BOOST_TEST((commands.find("shape.create")->risk == cmd::danger::none));
	BOOST_TEST((commands.find("shape.rebuild")->risk == cmd::danger::confirm));
	BOOST_TEST(static_cast<bool>(commands.find("shape.rebuild")->describe));
}

BOOST_AUTO_TEST_CASE(Create_WritesTheTwoFiles_InTheFormOfTheRepositoryDescs)
{
	const auto made = create_sphere();
	BOOST_REQUIRE_MESSAGE(made.ok, made.error);
	const shape_asset_files expected = built(small_sphere);
	BOOST_TEST(read_file(res_root / "shapes" / "ball.poly.desc") == expected.poly);
	BOOST_TEST(same_json(read_file(res_root / "shapes" / "ball.geom.desc"), expected.geometry));
	BOOST_TEST(read_file(res_root / "shapes" / "ball.geom.bin") == std::string(reinterpret_cast<const char*>(expected.binary.data()), expected.binary.size()));
	BOOST_TEST(std::string(made.value.at("poly").as_string()) == "res://shapes/ball.poly.desc");
	BOOST_TEST(std::string(made.value.at("geometry").as_string()) == "res://shapes/ball.geom.desc");
	BOOST_TEST(std::string(made.value.at("version").as_string()) == version_of_text(expected.poly));
	BOOST_TEST(made.value.at("vertices").to_number<std::size_t>() == expected.vertices);
	BOOST_REQUIRE_EQUAL(actions.size(), 1u);
	BOOST_TEST(actions[0].command == "shape.create");
	BOOST_TEST(actions[0].target == "shapes/ball.poly.desc");
}

BOOST_AUTO_TEST_CASE(Create_TakesAFolder_AndNeverReplacesAFile_OrShadowsALowerLayersOne)
{
	const auto in_folder = call("shape.create", json::object{ { "name", "crate" }, { "kind", "box" }, { "folder", "props/shapes" },
		{ "params", json::object{ { "size", json::array{ 2.0, 1.0, 1.0 } } } } });
	BOOST_REQUIRE_MESSAGE(in_folder.ok, in_folder.error);
	BOOST_TEST(fs::exists(res_root / "props" / "shapes" / "crate.poly.desc"));

	const auto again = call("shape.create", json::object{ { "name", "crate" }, { "kind", "box" }, { "folder", "props/shapes" } });
	BOOST_TEST(again.code == cmd::codes::needs_input);

	write_file(lower / "shapes" / "box.geom.desc", "{}");
	const auto shadow = call("shape.create", json::object{ { "name", "box" }, { "kind", "box" } });
	BOOST_TEST(shadow.code == cmd::codes::needs_input);
	BOOST_TEST(!fs::exists(res_root / "shapes" / "box.poly.desc"));
}

BOOST_AUTO_TEST_CASE(Create_RefusesWhatTheGeneratorRefuses_ABadPath_ABusyProject_AndAShapeTooLargeToBeRead_AndWritesNothing)
{
	const auto too_many = call("shape.create", json::object{ { "name", "a" }, { "kind", "sphere" }, { "params", json::object{ { "segments", 1000 } } } });
	BOOST_TEST(too_many.code == cmd::codes::invalid_args);
	const auto unknown_field = call("shape.create", json::object{ { "name", "a" }, { "kind", "box" }, { "params", json::object{ { "size", "big" } } } });
	BOOST_TEST(unknown_field.code == cmd::codes::invalid_args);
	BOOST_TEST(call("shape.create", json::object{ { "name", "a" }, { "kind", "torus" } }).code == cmd::codes::invalid_args);
	BOOST_TEST(call("shape.create", json::object{ { "name", "../a" }, { "kind", "box" } }).code == cmd::codes::invalid_args);
	BOOST_TEST(call("shape.create", json::object{ { "name", "a" }, { "kind", "box" }, { "folder", "../out" } }).code == cmd::codes::invalid_args);

	const auto large = call("shape.create", json::object{ { "name", "big" }, { "kind", "sphere" },
		{ "params", json::object{ { "segments", 200 }, { "rings", 200 } } } });
	BOOST_TEST(large.code == cmd::codes::invalid_args);
	BOOST_TEST(large.error.find("too large") != std::string::npos);

	blocked = "a migration runs";
	BOOST_TEST(call("shape.create", json::object{ { "name", "a" }, { "kind", "box" } }).code == cmd::codes::busy);
	BOOST_TEST(!fs::exists(res_root / "shapes"));
	BOOST_TEST(actions.empty());
}

BOOST_AUTO_TEST_CASE(Rebuild_WritesNewParametersIntoBothFiles_AndKeepsTheStoredOnesWithoutArguments)
{
	const auto made = create_sphere();
	BOOST_REQUIRE_MESSAGE(made.ok, made.error);
	const std::string version = std::string(made.value.at("version").as_string());

	const auto same = call("shape.rebuild", rebuild_args("res://shapes/ball.poly.desc", version));
	BOOST_REQUIRE_MESSAGE(same.ok, same.error);
	BOOST_TEST(std::string(same.value.at("version").as_string()) == version);

	json::object args = rebuild_args("shapes/ball.poly.desc", version);
	args["params"] = json::object{ { "radius", 1.0 }, { "segments", 12 }, { "rings", 6 } };
	const auto bigger = call("shape.rebuild", args);
	BOOST_REQUIRE_MESSAGE(bigger.ok, bigger.error);
	const shape_asset_files expected = built(sphere_params{ 1.0f, 12, 6 });
	BOOST_TEST(read_file(res_root / "shapes" / "ball.poly.desc") == expected.poly);
	BOOST_TEST(same_json(read_file(res_root / "shapes" / "ball.geom.desc"), expected.geometry));
	BOOST_TEST(std::string(bigger.value.at("version").as_string()) == version_of_text(expected.poly));

	json::object other_kind = rebuild_args("shapes/ball.poly.desc", std::string(bigger.value.at("version").as_string()));
	other_kind["kind"] = "box";
	BOOST_REQUIRE(call("shape.rebuild", other_kind).ok);
	BOOST_TEST(same_json(read_file(res_root / "shapes" / "ball.geom.desc"), built(box_params{}).geometry));
	BOOST_TEST(actions.back().command == "shape.rebuild");
}

BOOST_AUTO_TEST_CASE(Rebuild_RefusesAStaleVersion_AMissingFile_ALowerLayersFile_AndABusyProject)
{
	const auto made = create_sphere();
	BOOST_REQUIRE(made.ok);
	const std::string version = std::string(made.value.at("version").as_string());
	const std::string before = read_file(res_root / "shapes" / "ball.geom.desc");

	BOOST_TEST(call("shape.rebuild", rebuild_args("shapes/ball.poly.desc", "old")).code == cmd::codes::stale);
	BOOST_TEST(call("shape.rebuild", rebuild_args("shapes/none.poly.desc", version)).code == cmd::codes::not_found);
	BOOST_TEST(call("shape.rebuild", rebuild_args("shapes/ball.geom.desc", version)).code == cmd::codes::invalid_args);
	BOOST_TEST(call("shape.rebuild", rebuild_args("shapes/../ball.poly.desc", version)).code == cmd::codes::invalid_args);

	const shape_asset_files lower_files = built(box_params{});
	write_file(lower / "shapes" / "core.poly.desc", lower_files.poly);
	BOOST_TEST(call("shape.rebuild", rebuild_args("shapes/core.poly.desc", version_of_text(lower_files.poly))).code == cmd::codes::invalid_args);

	blocked = "a migration runs";
	BOOST_TEST(call("shape.rebuild", rebuild_args("shapes/ball.poly.desc", version)).code == cmd::codes::busy);
	BOOST_TEST(read_file(res_root / "shapes" / "ball.geom.desc") == before);
}

BOOST_AUTO_TEST_CASE(Rebuild_RefusesAMeshEditedByHand_AndLeavesBothFilesAsTheyAre)
{
	BOOST_REQUIRE(create_sphere().ok);
	const fs::path poly = res_root / "shapes" / "ball.poly.desc";
	json::value moved = json::parse(read_file(poly));
	moved.as_object().at("vertices").as_array()[0].as_array()[1] = 0.25;
	const std::string edited = json::serialize(moved);
	write_file(poly, edited);
	const std::string geometry = read_file(res_root / "shapes" / "ball.geom.desc");

	const auto refused = call("shape.rebuild", rebuild_args("shapes/ball.poly.desc", version_of_text(edited)));
	BOOST_TEST(refused.code == cmd::codes::failed);
	BOOST_TEST(refused.error.find("edited by hand") != std::string::npos);
	BOOST_TEST(read_file(poly) == edited);
	BOOST_TEST(read_file(res_root / "shapes" / "ball.geom.desc") == geometry);
}

BOOST_AUTO_TEST_CASE(Rebuild_MakesAGeometryFileThatIsGone_AndTheQuestionNamesTheFiles)
{
	const auto made = create_sphere();
	BOOST_REQUIRE(made.ok);
	const std::string version = std::string(made.value.at("version").as_string());
	fs::remove(res_root / "shapes" / "ball.geom.desc");

	const cmd::command_desc* rebuild = commands.find("shape.rebuild");
	cmd::call question;
	question.args = rebuild_args("shapes/ball.poly.desc", version);
	question.from = cmd::origin::agent("s1");
	const std::optional<std::string> text = rebuild->describe(question);
	BOOST_REQUIRE(text.has_value());
	BOOST_TEST(text->find("res://shapes/ball.poly.desc") != std::string::npos);
	BOOST_TEST(text->find("res://shapes/ball.geom.desc") != std::string::npos);
	BOOST_TEST(text->find("Rebuild the shape") == 0u);

	BOOST_REQUIRE(call("shape.rebuild", rebuild_args("shapes/ball.poly.desc", version)).ok);
	BOOST_TEST(same_json(read_file(res_root / "shapes" / "ball.geom.desc"), built(small_sphere).geometry));

	question.args = rebuild_args("shapes/none.poly.desc", version);
	const std::optional<std::string> refused = rebuild->describe(question);
	BOOST_REQUIRE(refused.has_value());
	BOOST_TEST(refused->find("This request will be refused") == 0u);
}

BOOST_AUTO_TEST_CASE(Rebuild_KeepsTheStoredFieldsAMergingCallLeavesOut)
{
	const auto made = create_sphere();
	BOOST_REQUIRE(made.ok);
	std::string version = std::string(made.value.at("version").as_string());

	json::object radius_only = rebuild_args("shapes/ball.poly.desc", version);
	radius_only["params"] = json::object{ { "radius", 2.0 } };
	const auto wider = call("shape.rebuild", radius_only);
	BOOST_REQUIRE_MESSAGE(wider.ok, wider.error);
	BOOST_TEST(read_file(res_root / "shapes" / "ball.poly.desc") == built(sphere_params{ 2.0f, 8, 4 }).poly);

	json::object same_kind = rebuild_args("shapes/ball.poly.desc", std::string(wider.value.at("version").as_string()));
	same_kind["kind"] = "sphere";
	const auto kept = call("shape.rebuild", same_kind);
	BOOST_REQUIRE_MESSAGE(kept.ok, kept.error);
	BOOST_TEST(read_file(res_root / "shapes" / "ball.poly.desc") == built(sphere_params{ 2.0f, 8, 4 }).poly);

	json::object other = rebuild_args("shapes/ball.poly.desc", std::string(kept.value.at("version").as_string()));
	other["kind"] = "cylinder";
	other["params"] = json::object{ { "height", 3.0 } };
	BOOST_REQUIRE(call("shape.rebuild", other).ok);
	BOOST_TEST(read_file(res_root / "shapes" / "ball.poly.desc") == built(cylinder_params{ 0.5f, 3.f, 16 }).poly);
}

BOOST_AUTO_TEST_CASE(Rebuild_ReportsTheRangeAndLetsTheSceneMoveItsNodes)
{
	const auto made = create_sphere();
	BOOST_REQUIRE(made.ok);
	updated_nodes = 3;
	json::object args = rebuild_args("shapes/ball.poly.desc", std::string(made.value.at("version").as_string()));
	args["params"] = json::object{ { "segments", 12 }, { "rings", 6 } };
	const auto rebuilt = call("shape.rebuild", args);
	BOOST_REQUIRE_MESSAGE(rebuilt.ok, rebuilt.error);
	const shape_asset_files expected = built(sphere_params{ 0.5f, 12, 6 });
	BOOST_TEST(rebuilt.value.at("vx_end").to_number<std::size_t>() == expected.vertices);
	BOOST_TEST(rebuilt.value.at("ind_end").to_number<std::size_t>() == expected.indices);
	BOOST_TEST(rebuilt.value.at("nodes_updated").to_number<std::size_t>() == 3u);
	BOOST_TEST(!rebuilt.value.as_object().contains("scene_update"));
	BOOST_TEST(std::string(updated_with.at("geometry").as_string()) == "res://shapes/ball.geom.desc");
	BOOST_TEST(updated_with.at("vx_end").to_number<std::size_t>() == expected.vertices);
	BOOST_TEST(updated_with.at("ind_begin").to_number<int>() == 0);
}

BOOST_AUTO_TEST_CASE(Rebuild_SaysWhyTheSceneWasNotUpdated_AndStillRebuildsTheFiles)
{
	const auto made = create_sphere();
	BOOST_REQUIRE(made.ok);
	registrations.clear();
	registrations = register_shape_commands(commands, shape_command_deps{ *rs, *descs, [this] {
		edt::desc_file_layout layout;
		layout.top_layer = [this] { return res_root; };
		layout.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		return layout;
	}(), commands, &events });
	json::object args = rebuild_args("shapes/ball.poly.desc", std::string(made.value.at("version").as_string()));
	args["params"] = json::object{ { "radius", 1.0 } };
	const auto rebuilt = call("shape.rebuild", args);
	BOOST_REQUIRE_MESSAGE(rebuilt.ok, rebuilt.error);
	BOOST_TEST(rebuilt.value.at("nodes_updated").to_number<int>() == 0);
	BOOST_TEST(!std::string(rebuilt.value.at("scene_update").as_string()).empty());
}

BOOST_AUTO_TEST_CASE(Add_MakesANodeWithTheRangeOfTheGeometryFile_AndItsSize)
{
	const auto made = create_sphere();
	BOOST_REQUIRE(made.ok);
	const shape_asset_files files = built(small_sphere);
	const json::value parent = json::parse(R"({"context":"w0","path":[]})");
	const auto added = call("shape.add", json::object{ { "poly", "res://shapes/ball.poly.desc" }, { "parent", parent },
		{ "scale", json::array{ 2.0, 3.0, 4.0 } }, { "name", "Ball" } });
	BOOST_REQUIRE_MESSAGE(added.ok, added.error);
	BOOST_TEST(std::string(created.at("name").as_string()) == "Ball");
	BOOST_TEST(created.at("scale").as_array().size() == 3u);
	const json::object& fields = created.at("components").as_object().at("mesh").as_object().at("fields").as_object();
	BOOST_TEST(std::string(fields.at("geometry").as_string()) == "res://shapes/ball.geom.desc");
	BOOST_TEST(fields.at("vx_begin").to_number<int>() == 0);
	BOOST_TEST(fields.at("vx_end").to_number<std::size_t>() == files.vertices);
	BOOST_TEST(fields.at("ind_end").to_number<std::size_t>() == files.indices);

	const auto unnamed = call("shape.add", json::object{ { "poly", "shapes/ball.poly.desc" }, { "parent", parent } });
	BOOST_REQUIRE_MESSAGE(unnamed.ok, unnamed.error);
	BOOST_TEST(std::string(created.at("name").as_string()) == "ball");
	BOOST_TEST(!created.contains("scale"));
}

BOOST_AUTO_TEST_CASE(Add_RefusesAMissingShape_ABadPath_AFlippedScale_AMissingGeometry_AndAPolyThatIsNoMesh)
{
	const json::value parent = json::parse(R"({"context":"w0","path":[]})");
	BOOST_TEST(call("shape.add", json::object{ { "poly", "shapes/none.poly.desc" }, { "parent", parent } }).code == cmd::codes::not_found);
	BOOST_TEST(call("shape.add", json::object{ { "poly", "shapes/ball.geom.desc" }, { "parent", parent } }).code == cmd::codes::invalid_args);
	BOOST_REQUIRE(create_sphere().ok);
	for (const json::value& scale : { json::value(json::array{ 1.0, -1.0, 1.0 }), json::value(json::array{ 0.0, 1.0, 1.0 }), json::value(json::array{ 1.0, 1.0 }) }) {
		BOOST_TEST(call("shape.add", json::object{ { "poly", "shapes/ball.poly.desc" }, { "parent", parent }, { "scale", scale } }).code == cmd::codes::invalid_args);
	}
	fs::remove(res_root / "shapes" / "ball.geom.desc");
	BOOST_TEST(call("shape.add", json::object{ { "poly", "shapes/ball.poly.desc" }, { "parent", parent } }).code == cmd::codes::not_found);
	write_file(res_root / "shapes" / "ball.geom.desc", "{}");
	write_file(res_root / "shapes" / "ball.poly.desc", "{}");
	BOOST_TEST(call("shape.add", json::object{ { "poly", "shapes/ball.poly.desc" }, { "parent", parent } }).code == cmd::codes::failed);
	BOOST_TEST(created.empty());
}

BOOST_AUTO_TEST_CASE(Create_RefusesAShapeTooLargeBeforeItIsMade_AndTheSmallestOnesPass)
{
	const auto huge = call("shape.create", json::object{ { "name", "huge" }, { "kind", "capsule" },
		{ "params", json::object{ { "segments", 256 }, { "hemisphere_rings", 256 } } } });
	BOOST_TEST(huge.code == cmd::codes::invalid_args);
	BOOST_TEST(huge.error.find("too large") != std::string::npos);
	BOOST_TEST(!fs::exists(res_root / "shapes"));

	for (const json::object& tiny : { json::object{ { "name", "t1" }, { "kind", "sphere" }, { "params", json::object{ { "segments", 3 }, { "rings", 2 } } } },
			json::object{ { "name", "t2" }, { "kind", "cylinder" }, { "params", json::object{ { "segments", 3 } } } },
			json::object{ { "name", "t3" }, { "kind", "capsule" }, { "params", json::object{ { "segments", 3 }, { "hemisphere_rings", 1 } } } } }) {
		BOOST_TEST_CONTEXT(std::string(tiny.at("kind").as_string())) {
			const auto made = call("shape.create", tiny);
			BOOST_TEST(made.ok, made.error);
		}
	}
	for (const json::object& broken : { json::object{ { "name", "b1" }, { "kind", "sphere" }, { "params", json::object{ { "segments", 2 } } } },
			json::object{ { "name", "b2" }, { "kind", "sphere" }, { "params", json::object{ { "rings", 1 } } } },
			json::object{ { "name", "b3" }, { "kind", "box" }, { "params", json::object{ { "size", json::array{ 1.0, 0.0, 1.0 } } } } },
			json::object{ { "name", "b4" }, { "kind", "box" }, { "params", json::object{ { "size", json::array{ 1.0, -1.0, 1.0 } } } } },
			json::object{ { "name", "b5" }, { "kind", "cylinder" }, { "params", json::object{ { "radius", 0.0 } } } } }) {
		BOOST_TEST_CONTEXT(std::string(broken.at("name").as_string())) {
			BOOST_TEST(call("shape.create", broken).code == cmd::codes::invalid_args);
		}
	}
	BOOST_TEST(!fs::exists(res_root / "shapes" / "b1.poly.desc"));
}

BOOST_AUTO_TEST_CASE(Create_SaysABuiltInNameIsTaken)
{
	write_file(lower / "shapes" / "box.poly.desc", "{}");
	const auto clash = call("shape.create", json::object{ { "name", "box" }, { "kind", "box" } });
	BOOST_TEST(clash.code == cmd::codes::needs_input);
	BOOST_TEST(clash.error.find("choose another name") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
