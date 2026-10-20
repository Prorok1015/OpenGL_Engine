#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_mcp_bridge_host.h"
#include "edt_scene_document.h"
#include <snk/command/cmd_registry.h>
#include <snk/desc/desc_json_schema.h>
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_selection_service.h>
#include <snk/editor/edt_state_commands.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

// scene.query, node.get, component.schema, asset.query and selection.get over a small scene (A4, #290): refs,
// filters, pagination, the page_size cap, a cursor that goes stale after an edit, schema output of migrated types.

namespace {

namespace json = boost::json;
namespace fs = std::filesystem;

constexpr const char* WORLD = R"({
	"__type": "world_desc", "__version": 3, "name": "w0", "systems": [],
	"children": [
		{ "__id": "00000000000000a1", "name": "Sun",
		  "components": { "directional_light_desc": { "__type": "directional_light_desc", "direction": [0.0, -1.0, 0.0, 0.0] } } },
		{ "__id": "00000000000000a2", "name": "MainCamera",
		  "components": { "camera_desc": { "__type": "camera_desc", "fov": 70.0 } } },
		{ "__id": "00000000000000a3", "name": "Crate",
		  "children": [ { "__id": "00000000000000b1", "name": "Lid" } ] },
		{ "__id": "00000000000000a4", "name": "Crate2" }
	]
})";

// Two resource layers on disk: the upper one shadows models/ship.glb of the lower one.
struct temp_layers
{
	temp_layers()
	{
		root = fs::temp_directory_path() / ("snk_state_commands_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
		fs::remove_all(root);
		const fs::path low = root / "low";
		const fs::path top = root / "top";
		put(low / "a.txt", "abc");
		put(low / "models" / "ship.glb", "old");
		put(low / "prefabs" / "crate.prefab.desc", "{}");
		put(top / "models" / "ship.glb", "newer!!");
		put(top / "z.txt", "z");
		layers = { low, top };
	}

	~temp_layers()
	{
		std::error_code ec;
		fs::remove_all(root, ec);
	}

	static void put(const fs::path& path, const std::string& text)
	{
		fs::create_directories(path.parent_path());
		std::ofstream file(path, std::ios::binary);
		file << text;
	}

	fs::path root;
	std::vector<fs::path> layers;
};

struct state_fixture : test_util::scene_env
{
	state_fixture()
	{
		auto world = load<scn::world_desc>(put("w.desc", WORLD));
		BOOST_REQUIRE(world);
		scene.world_descs.push_back(*world);
		scene.world_names.push_back("w0");
		scene.active_world_idx = 0;

		edt::asset_kind_desc prefab_kind;
		prefab_kind.id = "prefab";
		prefab_kind.suffix = ".prefab.desc";
		prefab_kind.open = [](edt::editor_context&, const res::tag&) {};
		kind_registration = kinds.add(std::move(prefab_kind));

		edt::state_command_sources sources{ documents, selection, *descs, *assembler, kinds, [this] { return disk.layers; } };
		registrations = edt::register_state_commands(commands, sources);
	}

	cmd::result call(const std::string& id, const std::string& args = "{}")
	{
		return commands.invoke(id, json::parse(args), cmd::origin::test());
	}

	static std::string ref(std::vector<std::string> ids, const std::string& world = "w0")
	{
		std::string path;
		for (const auto& id : ids) path += (path.empty() ? "" : ",") + std::string("\"") + id + "\"";
		return R"({"context":")" + world + R"(","path":[)" + path + "]}";
	}

	static std::vector<std::string> names_of(const cmd::result& result)
	{
		std::vector<std::string> names;
		for (const auto& item : result.value.as_object().at("items").as_array()) {
			names.emplace_back(item.at_pointer("/name").as_string().c_str());
		}
		return names;
	}

	static std::vector<std::string> names_of_tags(const cmd::result& result)
	{
		std::vector<std::string> tags;
		for (const auto& item : result.value.as_object().at("items").as_array()) {
			tags.emplace_back(item.at_pointer("/tag").as_string().c_str());
		}
		return tags;
	}

	static std::string cursor_of(const cmd::result& result)
	{
		const auto* cursor = result.value.as_object().if_contains("next_cursor");
		return cursor ? std::string(cursor->as_string().c_str()) : std::string{};
	}

	static const json::value& at(const cmd::result& result, const char* pointer)
	{
		boost::system::error_code ec;
		const json::value* found = result.value.find_pointer(pointer, ec);
		BOOST_REQUIRE_MESSAGE(found != nullptr, std::string("missing ") + pointer + " in " + json::serialize(result.value));
		return *found;
	}

	temp_layers disk;
	edt::scene_document scene;
	edt::editor_events events;
	edt::document_manager documents{ scene, events, *rs, *descs };
	edt::selection_service selection{ events };
	edt::asset_kind_registry kinds;
	edt::registration kind_registration;
	cmd::command_registry commands;
	std::vector<edt::registration> registrations;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtStateCommandsTests, state_fixture)

BOOST_AUTO_TEST_CASE(AllFiveCommandsAreRegisteredReadOnlyAndOfferedToAgents)
{
	for (const char* id : { "scene.query", "node.get", "component.schema", "asset.query", "selection.get" }) {
		BOOST_TEST_CONTEXT(id) {
			const cmd::command_desc* command = commands.find(id);
			BOOST_REQUIRE(command != nullptr);
			BOOST_TEST(!command->mutates);
			BOOST_TEST(!command->long_running);
			BOOST_TEST((command->risk == cmd::danger::none));
			BOOST_TEST(edt::agent_may_use(*command));
		}
	}
}

BOOST_AUTO_TEST_CASE(SceneQuery_ListsTheNodesDepthFirstWithRefsAndComponentKeys)
{
	const auto result = call("scene.query");

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_TEST((names_of(result) == std::vector<std::string>{ "Sun", "MainCamera", "Crate", "Lid", "Crate2" }));
	BOOST_TEST(at(result, "/world").as_string() == "w0");
	BOOST_TEST(at(result, "/document").as_uint64() == scene.id());
	BOOST_TEST(at(result, "/generation").as_uint64() == scene.generation());
	BOOST_TEST(result.value.as_object().if_contains("next_cursor") == nullptr);

	BOOST_TEST(json::serialize(at(result, "/items/0/ref")) == R"({"context":"w0","path":["00000000000000a1"]})");
	BOOST_TEST(json::serialize(at(result, "/items/0/components")) == R"(["directional_light_desc"])");
	BOOST_TEST(json::serialize(at(result, "/items/3/ref")) == R"({"context":"w0","path":["00000000000000a3","00000000000000b1"]})");
	BOOST_TEST(at(result, "/items/3/components").as_array().empty());
}

BOOST_AUTO_TEST_CASE(SceneQuery_FiltersByNameComponentAndSubtree)
{
	BOOST_TEST((names_of(call("scene.query", R"({"filter":{"name_glob":"crate"}})")) == std::vector<std::string>{ "Crate", "Crate2" }));
	BOOST_TEST((names_of(call("scene.query", R"({"filter":{"name_glob":"Cr*2"}})")) == std::vector<std::string>{ "Crate2" }));
	BOOST_TEST((names_of(call("scene.query", R"({"filter":{"name_glob":"?un"}})")) == std::vector<std::string>{ "Sun" }));
	BOOST_TEST((names_of(call("scene.query", R"({"filter":{"has_component":["camera_desc"]}})")) == std::vector<std::string>{ "MainCamera" }));
	BOOST_TEST(names_of(call("scene.query", R"({"filter":{"has_component":["camera_desc","directional_light_desc"]}})")).empty());
	BOOST_TEST((names_of(call("scene.query", R"({"filter":{"under":)" + ref({ "00000000000000a3" }) + "}}")) == std::vector<std::string>{ "Lid" }));
	BOOST_TEST((names_of(call("scene.query", R"({"filter":{"under":)" + ref({}) + "}}")).size() == 5u));
}

BOOST_AUTO_TEST_CASE(SceneQuery_PagesThroughTheNodesWithAnOpaqueCursor)
{
	std::vector<std::string> all;
	std::vector<std::size_t> sizes;
	std::string cursor;
	for (int page = 0; page < 10; ++page) {
		std::string args = R"({"page_size":2)";
		if (!cursor.empty()) args += R"(,"cursor":")" + cursor + "\"";
		const auto result = call("scene.query", args + "}");
		BOOST_REQUIRE_MESSAGE(result.ok, result.error);
		const auto names = names_of(result);
		sizes.push_back(names.size());
		all.insert(all.end(), names.begin(), names.end());
		cursor = cursor_of(result);
		if (cursor.empty()) break;
	}

	BOOST_TEST((sizes == std::vector<std::size_t>{ 2, 2, 1 }));
	BOOST_TEST((all == std::vector<std::string>{ "Sun", "MainCamera", "Crate", "Lid", "Crate2" }));
}

BOOST_AUTO_TEST_CASE(SceneQuery_PageSizeIsBounded)
{
	const auto too_big = call("scene.query", R"({"page_size":201})");
	BOOST_TEST(!too_big.ok);
	BOOST_TEST(too_big.code == cmd::codes::invalid_args);
	BOOST_TEST(too_big.error.find("page_size") != std::string::npos);

	BOOST_TEST(call("scene.query", R"({"page_size":0})").code == cmd::codes::invalid_args);
	BOOST_TEST(call("scene.query", R"({"page_size":-5})").code == cmd::codes::invalid_args);
	BOOST_TEST(call("scene.query", R"({"page_size":200})").ok);
	BOOST_TEST(call("asset.query", R"({"page_size":201})").code == cmd::codes::invalid_args);
	BOOST_TEST(edt::state_query_max_page_size == 200);

	// Without page_size the default applies, never "everything".
	BOOST_TEST(edt::state_query_default_page_size < edt::state_query_max_page_size);
}

BOOST_AUTO_TEST_CASE(SceneQuery_ACursorFromBeforeAnEditIsStale)
{
	const auto first = call("scene.query", R"({"page_size":2})");
	const std::string cursor = cursor_of(first);
	BOOST_REQUIRE(!cursor.empty());
	const std::string args = R"({"page_size":2,"cursor":")" + cursor + "\"}";
	BOOST_TEST(call("scene.query", args).ok);

	const auto generation = scene.generation();
	scene.touch_descs();

	const auto stale = call("scene.query", args);
	BOOST_TEST(!stale.ok);
	BOOST_TEST(stale.code == cmd::codes::stale);
	BOOST_TEST(names_of(stale).empty());

	const auto again = call("scene.query", R"({"page_size":2})");
	BOOST_REQUIRE(again.ok);
	BOOST_TEST(at(again, "/generation").as_uint64() == generation + 1);
	BOOST_TEST(call("scene.query", R"({"page_size":2,"cursor":")" + cursor_of(again) + "\"}").ok);
}

BOOST_AUTO_TEST_CASE(SceneQuery_ACursorOfAnotherQueryOrGarbageIsRefused)
{
	const std::string cursor = cursor_of(call("scene.query", R"({"page_size":2})"));
	BOOST_REQUIRE(!cursor.empty());

	const auto other = call("scene.query", R"({"page_size":2,"filter":{"name_glob":"c"},"cursor":")" + cursor + "\"}");
	BOOST_TEST(!other.ok);
	BOOST_TEST(other.code == cmd::codes::invalid_args);

	for (const char* garbage : { "x", "1.2.3", "1.2.3.zz", "" }) {
		BOOST_TEST_CONTEXT(garbage) {
			BOOST_TEST(call("scene.query", std::string(R"({"cursor":")") + garbage + "\"}").code == cmd::codes::invalid_args);
		}
	}
}

BOOST_AUTO_TEST_CASE(SceneQuery_UnknownWorldAndUnknownUnderAreNotFound)
{
	BOOST_TEST(call("scene.query", R"({"world":"nope"})").code == cmd::codes::not_found);
	BOOST_TEST(call("scene.query", R"({"filter":{"under":)" + ref({ "00000000000000ff" }) + "}}").code == cmd::codes::not_found);
	BOOST_TEST(call("scene.query", R"({"document":9999})").code == cmd::codes::not_found);
	BOOST_TEST(call("scene.query", std::string(R"({"document":)") + std::to_string(scene.id()) + "}").ok);
}

BOOST_AUTO_TEST_CASE(NodeGet_ReturnsTheNodeItsComponentsAsDescJsonAndItsChildren)
{
	const auto sun = call("node.get", R"({"ref":)" + ref({ "00000000000000a1" }) + "}");
	BOOST_REQUIRE_MESSAGE(sun.ok, sun.error);
	BOOST_TEST(at(sun, "/name").as_string() == "Sun");
	BOOST_TEST(at(sun, "/parent/path").as_array().empty());
	BOOST_TEST(at(sun, "/in_instance").as_bool() == false);
	BOOST_TEST(at(sun, "/children").as_array().empty());
	BOOST_TEST(at(sun, "/components/directional_light_desc/__type").as_string() == "directional_light_desc");
	BOOST_REQUIRE(at(sun, "/components/directional_light_desc/direction").is_array());
	BOOST_TEST(at(sun, "/components/directional_light_desc/direction/1").to_number<double>() == -1.0);

	const auto crate = call("node.get", R"({"ref":)" + ref({ "00000000000000a3" }) + "}");
	BOOST_REQUIRE_MESSAGE(crate.ok, crate.error);
	BOOST_TEST(at(crate, "/child_count").as_uint64() == 1u);
	BOOST_TEST(json::serialize(at(crate, "/children/0/ref")) == R"({"context":"w0","path":["00000000000000a3","00000000000000b1"]})");
	BOOST_TEST(at(crate, "/children/0/name").as_string() == "Lid");
	BOOST_TEST(at(crate, "/children_truncated").as_bool() == false);
	BOOST_TEST(at(crate, "/components").as_object().empty());

	// The ref a reply carries goes back in as it is.
	const auto lid = call("node.get", R"({"ref":)" + json::serialize(at(crate, "/children/0/ref")) + "}");
	BOOST_REQUIRE_MESSAGE(lid.ok, lid.error);
	BOOST_TEST(at(lid, "/name").as_string() == "Lid");
	BOOST_TEST(json::serialize(at(lid, "/parent")) == R"({"context":"w0","path":["00000000000000a3"]})");
}

BOOST_AUTO_TEST_CASE(NodeGet_TheWorldRootIsTheEmptyPathAndComponentsCanBeFiltered)
{
	const auto root = call("node.get", R"({"ref":)" + ref({}) + "}");
	BOOST_REQUIRE_MESSAGE(root.ok, root.error);
	BOOST_TEST(at(root, "/name").as_string() == "w0");
	BOOST_TEST(at(root, "/parent").is_null());
	BOOST_TEST(at(root, "/child_count").as_uint64() == 4u);

	const auto camera = call("node.get", R"({"ref":)" + ref({ "00000000000000a2" }) + R"(,"components":["camera_desc"]})");
	BOOST_REQUIRE_MESSAGE(camera.ok, camera.error);
	BOOST_TEST(at(camera, "/components").as_object().size() == 1u);

	const auto none = call("node.get", R"({"ref":)" + ref({ "00000000000000a2" }) + R"(,"components":["directional_light_desc"]})");
	BOOST_REQUIRE_MESSAGE(none.ok, none.error);
	BOOST_TEST(at(none, "/components").as_object().empty());
}

BOOST_AUTO_TEST_CASE(NodeGet_AnUnknownOrMalformedRefIsAnError)
{
	const auto missing = call("node.get", R"({"ref":)" + ref({ "00000000000000ff" }) + "}");
	BOOST_TEST(!missing.ok);
	BOOST_TEST(missing.code == cmd::codes::not_found);

	BOOST_TEST(call("node.get", R"({"ref":)" + ref({ "00000000000000a3", "00000000000000ee" }) + "}").code == cmd::codes::not_found);
	BOOST_TEST(call("node.get", R"({"ref":)" + ref({ "00000000000000a1" }, "nowhere") + "}").code == cmd::codes::not_found);
	BOOST_TEST(call("node.get", R"({"ref":)" + ref({ "not-hex" }) + "}").code == cmd::codes::invalid_args);
	BOOST_TEST(call("node.get", R"({"ref":{"context":"w0"}})").code == cmd::codes::invalid_args);
	BOOST_TEST(call("node.get").code == cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(NodeGet_ALevelPrefixedContextIsReadAsItsWorld)
{
	const auto result = call("node.get", R"({"ref":)" + ref({ "00000000000000a1" }, "level/w0") + "}");
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_TEST(at(result, "/ref/context").as_string() == "w0");
}

BOOST_AUTO_TEST_CASE(SelectionGet_ReportsTheSelectionAsRefsAndChangesNothing)
{
	const auto none = call("selection.get");
	BOOST_REQUIRE_MESSAGE(none.ok, none.error);
	BOOST_TEST(at(none, "/items").as_array().empty());

	edt::selection chosen;
	chosen.document = scene.id();
	chosen.world = "w0";
	chosen.path = { 0xa3, 0xb1 };
	selection.select(chosen);

	const auto generation = scene.generation();
	const auto one = call("selection.get");
	BOOST_REQUIRE_MESSAGE(one.ok, one.error);
	BOOST_REQUIRE(at(one, "/items").as_array().size() == 1u);
	BOOST_TEST(json::serialize(at(one, "/items/0/ref")) == R"({"context":"w0","path":["00000000000000a3","00000000000000b1"]})");
	BOOST_TEST(at(one, "/items/0/name").as_string() == "Lid");
	BOOST_TEST(at(one, "/items/0/readonly").as_bool() == false);
	BOOST_TEST(at(one, "/document").as_uint64() == scene.id());

	// The ref goes straight into node.get.
	BOOST_TEST(call("node.get", R"({"ref":)" + json::serialize(at(one, "/items/0/ref")) + "}").ok);

	BOOST_TEST(selection.current().path == chosen.path);
	BOOST_TEST(scene.generation() == generation);
	BOOST_TEST(!scene.is_dirty());

	selection.clear();
	BOOST_TEST(at(call("selection.get"), "/items").as_array().empty());
}

BOOST_AUTO_TEST_CASE(ReadCommands_LeaveTheDocumentAlone)
{
	const auto generation = scene.generation();
	call("scene.query");
	call("node.get", R"({"ref":)" + ref({ "00000000000000a3" }) + "}");
	call("component.schema", R"({"type":"camera_desc"})");
	call("asset.query");
	call("selection.get");

	BOOST_TEST(scene.generation() == generation);
	BOOST_TEST(!scene.is_dirty());
	BOOST_TEST(scene.world_descs[0].get_root().children.size() == 4u);
}

BOOST_AUTO_TEST_CASE(ComponentSchema_OfTheLightIsGeneratedFromItsTypeSchema)
{
	const auto result = call("component.schema", R"({"type":"directional_light_desc"})");

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_TEST(at(result, "/type").as_string() == "directional_light_desc");
	BOOST_TEST(at(result, "/is_component").as_bool());
	BOOST_TEST(at(result, "/schema/type").as_string() == "object");
	BOOST_TEST(at(result, "/schema/properties/__type/const").as_string() == "directional_light_desc");
	for (const char* field : { "direction", "diffuse", "ambient", "specular" }) {
		BOOST_TEST_CONTEXT(field) {
			const std::string base = std::string("/schema/properties/") + field;
			BOOST_TEST(at(result, (base + "/type").c_str()).as_string() == "array");
			BOOST_TEST(at(result, (base + "/minItems").c_str()).to_number<int>() == 4);
			BOOST_TEST(at(result, (base + "/maxItems").c_str()).to_number<int>() == 4);
			BOOST_TEST(at(result, (base + "/items/type").c_str()).as_string() == "number");
		}
	}
	// field_attrs of the light: direction is bounded to [-1, 1].
	BOOST_TEST(at(result, "/schema/properties/direction/items/minimum").to_number<double>() == -1.0);
	BOOST_TEST(at(result, "/schema/properties/direction/items/maximum").to_number<double>() == 1.0);
}

BOOST_AUTO_TEST_CASE(ComponentSchema_OfTheCameraHasRangesDefaultsNestedKeysAndEnums)
{
	const auto result = call("component.schema", R"({"type":"camera_desc"})");

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_TEST(at(result, "/schema/properties/fov/type").as_string() == "number");
	BOOST_TEST(at(result, "/schema/properties/fov/minimum").to_number<double>() == 1.0);
	BOOST_TEST(at(result, "/schema/properties/fov/maximum").to_number<double>() == 179.0);
	BOOST_TEST(at(result, "/schema/properties/fov/default").to_number<double>() == 90.0);
	BOOST_TEST(at(result, "/schema/properties/fov/x-degrees").as_bool());
	BOOST_TEST(at(result, "/schema/properties/fov/description").as_string() == "Vertical field of view");
	// The key "/transform/rotation" is nested under "transform".
	BOOST_TEST(at(result, "/schema/properties/transform/type").as_string() == "object");
	BOOST_TEST(at(result, "/schema/properties/transform/properties/rotation/type").as_string() == "array");
	BOOST_TEST(at(result, "/schema/properties/transform/properties/position/minItems").to_number<int>() == 3);
	// An enumeration lists the names of its enum schema.
	const json::array& clears = at(result, "/schema/properties/clear/enum").as_array();
	bool has_depth = false;
	for (const auto& name : clears) has_depth = has_depth || name.as_string() == "depth";
	BOOST_TEST(has_depth);
}

BOOST_AUTO_TEST_CASE(ComponentSchema_UnknownAndSchemalessTypesAreNotFound)
{
	const auto unknown = call("component.schema", R"({"type":"no_such_desc"})");
	BOOST_TEST(!unknown.ok);
	BOOST_TEST(unknown.code == cmd::codes::not_found);

	// stat_desc is a hand-written desc of the tests: registered, but not on the generic schema.
	const auto legacy = call("component.schema", R"({"type":"stat_desc"})");
	BOOST_TEST(!legacy.ok);
	BOOST_TEST(legacy.code == cmd::codes::not_found);
	BOOST_TEST(legacy.error.find("no schema") != std::string::npos);

	BOOST_TEST(call("component.schema", R"({"type":""})").code == cmd::codes::invalid_args);
	BOOST_TEST(call("component.schema").code == cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(JsonSchemaGenerator_WorksOnASchemaWithoutADesc)
{
	desc::schema_registry registry;
	desc::type_schema schema;
	schema.name = "t";
	desc::field_schema count;
	count.name = "count";
	count.key = "/count";
	count.kind = desc::field_kind::u32;
	count.range = desc::field_range{ 0.0, 10.0, 1.0 };
	desc::field_schema tags;
	tags.name = "tags";
	tags.key = "/tags";
	tags.kind = desc::field_kind::array;
	tags.element = desc::field_kind::string;
	schema.fields = { count, tags };

	const json::object out = desc::to_json_schema(schema, registry);
	const json::value root = out;

	BOOST_TEST(root.at_pointer("/properties/count/type").as_string() == "integer");
	BOOST_TEST(root.at_pointer("/properties/count/maximum").to_number<double>() == 10.0);
	BOOST_TEST(root.at_pointer("/properties/tags/type").as_string() == "array");
	BOOST_TEST(root.at_pointer("/properties/tags/items/type").as_string() == "string");
	BOOST_TEST(root.at_pointer("/title").as_string() == "t");
	BOOST_TEST(!out.contains("required"));
}

BOOST_AUTO_TEST_CASE(AssetQuery_ListsTheMergedLayersSortedWithKindSizeAndLayer)
{
	const auto result = call("asset.query");

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_TEST((names_of_tags(result) == std::vector<std::string>{
		"res://a.txt", "res://models/ship.glb", "res://prefabs/crate.prefab.desc", "res://z.txt" }));
	// The upper layer wins for ship.glb: its size, layer 1.
	BOOST_TEST(at(result, "/items/1/size").as_uint64() == 7u);
	BOOST_TEST(at(result, "/items/1/layer").as_uint64() == 1u);
	BOOST_TEST(at(result, "/items/0/layer").as_uint64() == 0u);
	BOOST_TEST(at(result, "/items/0/size").as_uint64() == 3u);
	BOOST_TEST(at(result, "/items/2/kind").as_string() == "prefab");
	BOOST_TEST(at(result, "/items/0/kind").as_string() == "");
	BOOST_TEST(at(result, "/scan_truncated").as_bool() == false);
}

BOOST_AUTO_TEST_CASE(AssetQuery_FiltersByGlobKindAndDirectory)
{
	BOOST_TEST((names_of_tags(call("asset.query", R"({"glob":"*.glb"})")) == std::vector<std::string>{ "res://models/ship.glb" }));
	BOOST_TEST((names_of_tags(call("asset.query", R"({"glob":"CRATE"})")) == std::vector<std::string>{ "res://prefabs/crate.prefab.desc" }));
	BOOST_TEST((names_of_tags(call("asset.query", R"({"kind":"prefab"})")) == std::vector<std::string>{ "res://prefabs/crate.prefab.desc" }));
	BOOST_TEST((names_of_tags(call("asset.query", R"({"directory":"models"})")) == std::vector<std::string>{ "res://models/ship.glb" }));
	BOOST_TEST(names_of_tags(call("asset.query", R"({"kind":"nothing"})")).empty());
	BOOST_TEST(names_of_tags(call("asset.query", R"({"directory":"missing"})")).empty());

	for (const char* bad : { "..", "../x", "models/../../x", "/etc" }) {
		BOOST_TEST_CONTEXT(bad) {
			BOOST_TEST(call("asset.query", std::string(R"({"directory":")") + bad + "\"}").code == cmd::codes::invalid_args);
		}
	}
}

BOOST_AUTO_TEST_CASE(AssetQuery_PagesWithAKeysetCursorThatSurvivesChangesOfTheAssets)
{
	const auto first = call("asset.query", R"({"page_size":3})");
	BOOST_REQUIRE_MESSAGE(first.ok, first.error);
	BOOST_TEST(names_of_tags(first).size() == 3u);
	const std::string cursor = cursor_of(first);
	BOOST_REQUIRE(!cursor.empty());

	// An asset added before the cursor moves no other row.
	temp_layers::put(disk.layers[1] / "0_first.txt", "x");
	const auto second = call("asset.query", R"({"page_size":3,"cursor":")" + cursor + "\"}");
	BOOST_REQUIRE_MESSAGE(second.ok, second.error);
	BOOST_TEST((names_of_tags(second) == std::vector<std::string>{ "res://z.txt" }));
	BOOST_TEST(cursor_of(second).empty());

	const auto other = call("asset.query", R"({"page_size":3,"glob":"txt","cursor":")" + cursor + "\"}");
	BOOST_TEST(other.code == cmd::codes::invalid_args);
	BOOST_TEST(call("asset.query", R"({"cursor":"nonsense"})").code == cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(AssetQuery_WithoutLayersIsEmpty)
{
	disk.layers.clear();
	const auto result = call("asset.query");
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_TEST(result.value.as_object().at("items").as_array().empty());
}

BOOST_AUTO_TEST_SUITE_END()
