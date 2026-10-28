#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_mcp_bridge_host.h"
#include "edt_scene_document.h"
#include <snk/command/cmd_registry.h>
#include <snk/desc/desc_json_schema.h>
#include <snk/editor/edt_asset_kind_registry.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_selection_service.h>
#include <snk/editor/edt_state_commands.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
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

// A second world: Inst instantiates a prefab with two nodes and holds override records for both.
constexpr const char* PREFAB = R"({
	"__type": "prefab_desc", "__version": 3,
	"children": [
		{ "__id": "00000000000000c1", "name": "arm", "components": { "stat": { "__type": "stat_desc", "hp": 3 } } },
		{ "__id": "00000000000000c2", "name": "leg", "components": { "stat": { "__type": "stat_desc", "hp": 4 } } }
	]
})";

constexpr const char* WORLD_1 = R"({
	"__type": "world_desc", "__version": 3, "name": "w1", "systems": [],
	"children": [
		{ "__id": "00000000000000e1", "name": "Inst",
		  "components": { "inst": { "__type": "prefab_desc", "__parent": "memory://t/crate.prefab.desc" } },
		  "overrides": [
			{ "target": ["00000000000000c1"], "component": "stat", "path": "/hp", "value": 9 },
			{ "target": ["00000000000000c2"], "component": "stat", "path": "/hp", "value": 5 } ] },
		{ "__id": "00000000000000e2", "name": "Other" }
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
		put("crate.prefab.desc", PREFAB);
		auto world_1 = load<scn::world_desc>(put("w1.desc", WORLD_1));
		BOOST_REQUIRE(world_1);
		scene.world_descs.push_back(*world_1);
		scene.world_names.push_back("w1");

		edt::asset_kind_desc prefab_kind;
		prefab_kind.id = "prefab";
		prefab_kind.suffix = ".prefab.desc";
		prefab_kind.open = [](edt::editor_context&, const res::tag&) {};
		kind_registration = kinds.add(std::move(prefab_kind));

		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		edt::state_command_sources sources{ documents, selection, services, kinds, [this] { return disk.layers; } };
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
		BOOST_REQUIRE_MESSAGE(result.ok && result.value.as_object().contains("items"), "no items in the result: " + result.error);
		for (const auto& item : result.value.as_object().at("items").as_array()) {
			names.emplace_back(item.at_pointer("/name").as_string().c_str());
		}
		return names;
	}

	static std::vector<std::string> names_of_tags(const cmd::result& result)
	{
		std::vector<std::string> tags;
		BOOST_REQUIRE_MESSAGE(result.ok && result.value.as_object().contains("items"), "no items in the result: " + result.error);
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
	edt::editor_services services;
	edt::scene_document scene;
	edt::editor_events events;
	edt::document_manager documents{ scene, services, events };
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
	BOOST_TEST(!stale.error.empty());
	BOOST_TEST(!stale.value.as_object().contains("items"));

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

BOOST_AUTO_TEST_CASE(NodeGet_ANodeInsideAPrefabInstanceIsTheBaseWithTheOwnersRecordsOnTop)
{
	const std::string inst = "00000000000000e1";

	const auto arm = call("node.get", R"({"ref":)" + ref({ inst, "00000000000000c1" }, "w1") + "}");
	BOOST_REQUIRE_MESSAGE(arm.ok, arm.error);
	BOOST_TEST(at(arm, "/in_instance").as_bool());
	BOOST_TEST(at(arm, "/name").as_string() == "arm");
	BOOST_TEST(at(arm, "/components/stat/__type").as_string() == "stat_desc");
	BOOST_TEST(at(arm, "/components/stat/hp").to_number<int>() == 9); // the prefab says 3
	BOOST_TEST(at(arm, "/overrides").as_array().size() == 1u);
	BOOST_TEST(json::serialize(at(arm, "/overrides/0/target")) == R"(["00000000000000c1"])");
	BOOST_TEST(at(arm, "/overrides/0/component").as_string() == "stat");
	BOOST_TEST(at(arm, "/transform").is_object());

	const auto leg = call("node.get", R"({"ref":)" + ref({ inst, "00000000000000c2" }, "w1") + "}");
	BOOST_REQUIRE_MESSAGE(leg.ok, leg.error);
	BOOST_TEST(at(leg, "/components/stat/hp").to_number<int>() == 5);
	BOOST_TEST(at(leg, "/overrides").as_array().size() == 1u);

	// The instance node itself: its prefab, the content as children, all of its records.
	const auto owner = call("node.get", R"({"ref":)" + ref({ inst }, "w1") + "}");
	BOOST_REQUIRE_MESSAGE(owner.ok, owner.error);
	BOOST_TEST(!at(owner, "/in_instance").as_bool());
	BOOST_TEST(at(owner, "/prefab").as_string() == "memory://t/crate.prefab.desc");
	BOOST_TEST(at(owner, "/components").as_object().empty());
	BOOST_TEST(at(owner, "/children").as_array().size() == 2u);
	BOOST_TEST(at(owner, "/children/0/name").as_string() == "arm");
	BOOST_TEST(at(owner, "/overrides").as_array().size() == 2u);

	BOOST_TEST(call("node.get", R"({"ref":)" + ref({ inst, "00000000000000ee" }, "w1") + "}").code == cmd::codes::not_found);
}

BOOST_AUTO_TEST_CASE(NodeGet_ListsAtMostTwoHundredChildrenAndSaysSo)
{
	scene.world_descs.push_back(scene.world_descs[0]);
	scene.world_names.push_back("big");
	auto& root = scene.world_descs.back().get_root();
	root.children.clear();
	for (std::uint64_t i = 1; i <= 205; ++i) {
		scn::prefab_desc::prefab_node node;
		node.name = "n" + std::to_string(i);
		node.node_id = 0x1000 + i;
		root.children.push_back(std::move(node));
	}

	const auto result = call("node.get", R"({"ref":)" + ref({}, "big") + "}");

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_TEST(at(result, "/child_count").as_uint64() == 205u);
	BOOST_TEST(at(result, "/children").as_array().size() == 200u);
	BOOST_TEST(at(result, "/children_truncated").as_bool());
}

BOOST_AUTO_TEST_CASE(SceneQuery_UnderMustBeANodeOfTheQueriedWorld)
{
	const std::string other_world_node = ref({ "00000000000000a3" }, "w0");

	const auto crossed = call("scene.query", R"({"world":"w1","filter":{"under":)" + other_world_node + "}}");
	BOOST_TEST(!crossed.ok);
	BOOST_TEST(crossed.code == cmd::codes::invalid_args);
	BOOST_TEST(call("scene.query", R"({"filter":{"under":)" + ref({ "00000000000000e1" }, "w1") + "}}").code == cmd::codes::invalid_args);

	const auto own = call("scene.query", R"({"world":"w1","filter":{"under":)" + ref({}, "w1") + "}}");
	BOOST_REQUIRE_MESSAGE(own.ok, own.error);
	BOOST_TEST((names_of(own) == std::vector<std::string>{ "Inst", "Other" }));
	BOOST_TEST(at(own, "/world").as_string() == "w1");
	BOOST_TEST(at(own, "/items/0/ref/context").as_string() == "w1");
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

BOOST_AUTO_TEST_CASE(ComponentSchema_ARangeWithoutSpanIsNoBound)
{
	// camera_desc /transform/rotation is range(0, 0, 0.5): only a drag step, no bounds.
	const auto result = call("component.schema", R"({"type":"camera_desc"})");
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	boost::system::error_code ec;
	BOOST_TEST(result.value.find_pointer("/schema/properties/transform/properties/rotation/items/minimum", ec) == nullptr);
	BOOST_TEST(result.value.find_pointer("/schema/properties/transform/properties/rotation/items/maximum", ec) == nullptr);
	BOOST_TEST(result.value.find_pointer("/schema/properties/transform/properties/rotation/minimum", ec) == nullptr);
	// A bounded field keeps both.
	BOOST_TEST(at(result, "/schema/properties/far_distance/minimum").to_number<double>() == 1.0);
	BOOST_TEST(at(result, "/schema/properties/far_distance/maximum").to_number<double>() == 100000.0);
}

BOOST_AUTO_TEST_CASE(JsonSchemaGenerator_ARangeOfAnArrayOfVectorsBoundsTheNumbers)
{
	desc::schema_registry registry;
	desc::type_schema schema;
	schema.name = "points";
	desc::field_schema points;
	points.name = "points";
	points.key = "/points";
	points.kind = desc::field_kind::array;
	points.element = desc::field_kind::vec3;
	points.range = desc::field_range{ 0.0, 5.0, 1.0 };
	desc::field_schema weights;
	weights.name = "weights";
	weights.key = "/weights";
	weights.kind = desc::field_kind::array;
	weights.element = desc::field_kind::f32;
	weights.range = desc::field_range{ 0.0, 1.0, 0.1 };
	schema.fields = { points, weights };

	const json::value root = desc::to_json_schema(schema, registry);

	boost::system::error_code ec;
	BOOST_TEST(root.at_pointer("/properties/points/items/items/minimum").to_number<double>() == 0.0);
	BOOST_TEST(root.at_pointer("/properties/points/items/items/maximum").to_number<double>() == 5.0);
	BOOST_TEST(root.find_pointer("/properties/points/items/minimum", ec) == nullptr);
	BOOST_TEST(root.at_pointer("/properties/weights/items/maximum").to_number<double>() == 1.0);
}

BOOST_AUTO_TEST_CASE(JsonSchemaGenerator_StructuresThatRefereToThemselvesStayBounded)
{
	desc::schema_registry registry;
	desc::type_schema node;
	node.name = "loop";
	for (const char* name : { "a", "b", "c", "d" }) {
		desc::field_schema field;
		field.name = name;
		field.key = std::string("/") + name;
		field.kind = desc::field_kind::structure;
		field.ref_type = "loop";
		node.fields.push_back(field);
	}
	const desc::type_schema* stored = registry.add(node);
	BOOST_REQUIRE(stored != nullptr);

	const json::object out = desc::to_json_schema(*stored, registry);

	// 4 fields per level, 8 levels deep is 87381 nodes unbounded; the budget stops it far earlier.
	BOOST_TEST(json::serialize(out).size() < 600000u);
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

BOOST_AUTO_TEST_CASE(AssetQuery_DirectoryAndPatternsCannotReachOutsideTheLayers)
{
	// An embedded NUL ends the path at the OS call: "..\0x" would be ".." there.
	for (const char* args : {
		R"({"directory":"..\u0000x"})",
		R"({"directory":"models\u0000"})",
		R"({"directory":"models\\..\\.."})",
		R"({"directory":"..\\"})",
		R"({"directory":"C:\\"})",
		R"({"directory":"c:"})",
		R"({"directory":"models:stream"})",
		R"({"directory":"..."})",
		R"({"directory":".. "})",
		R"({"directory":"res://../x"})",
		R"({"directory":"res:///etc"})",
		R"({"directory":"models\n"})",
		R"({"glob":"a\u0000b"})",
		R"({"kind":"\u0001"})" }) {
		BOOST_TEST_CONTEXT(args) {
			const auto result = call("asset.query", args);
			BOOST_TEST(!result.ok);
			BOOST_TEST(result.code == cmd::codes::invalid_args);
		}
	}
	BOOST_TEST(call("asset.query", std::format(R"({{"directory":"{}"}})", std::string(513, 'a')).c_str()).code == cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(Globs_AreBoundedAndCursorsMustBePlainText)
{
	const std::string many_stars = "a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*b";
	BOOST_TEST(call("asset.query", R"({"glob":")" + many_stars + "\"}").code == cmd::codes::invalid_args);
	BOOST_TEST(call("scene.query", R"({"filter":{"name_glob":")" + many_stars + "\"}}").code == cmd::codes::invalid_args);
	BOOST_TEST(call("asset.query", R"({"glob":")" + std::string(129, 'a') + "\"}").code == cmd::codes::invalid_args);
	BOOST_TEST(call("scene.query", R"({"filter":{"name_glob":")" + std::string(129, 'a') + "\"}}").code == cmd::codes::invalid_args);
	BOOST_TEST(call("scene.query", R"({"filter":{"name_glob":"a\u0000"}})").code == cmd::codes::invalid_args);

	// A run of stars is one star: the same answer, and it does not count against the limit.
	const std::string run(40, '*');
	BOOST_TEST((names_of_tags(call("asset.query", R"({"glob":")" + run + "ship" + run + "\"}")) == std::vector<std::string>{ "res://models/ship.glb" }));
	BOOST_TEST((names_of(call("scene.query", R"({"filter":{"name_glob":"C)" + run + R"(2"}})")) == std::vector<std::string>{ "Crate2" }));

	for (const char* cursor_json : { R"({"cursor":"1.2.3.\u0001"})", R"({"cursor":"a\\b"})" }) {
		BOOST_TEST_CONTEXT(cursor_json) {
			BOOST_TEST(call("scene.query", cursor_json).code == cmd::codes::invalid_args);
			BOOST_TEST(call("asset.query", cursor_json).code == cmd::codes::invalid_args);
		}
	}
	const std::string long_cursor = std::string(600, '1');
	BOOST_TEST(call("scene.query", R"({"cursor":")" + long_cursor + "\"}").code == cmd::codes::invalid_args);
	BOOST_TEST(call("asset.query", R"({"cursor":")" + long_cursor + "\"}").code == cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(Replies_NeverCarryBytesThatAreNotUtf8)
{
	scene.world_descs[0].get_root().children[0].name = "Su\xff" "n";

	const auto query = call("scene.query");
	BOOST_REQUIRE_MESSAGE(query.ok, query.error);
	BOOST_TEST(at(query, "/items/0/name").as_string() == "Su?n");
	const auto node = call("node.get", R"({"ref":)" + ref({ "00000000000000a1" }) + "}");
	BOOST_REQUIRE_MESSAGE(node.ok, node.error);
	BOOST_TEST(at(node, "/name").as_string() == "Su?n");
	// What the reply holds must survive a round trip through the JSON text.
	BOOST_TEST(json::parse(json::serialize(query.value)).is_object());
}

BOOST_AUTO_TEST_CASE(AssetQuery_ADirectoryMayCarryTheResPrefix)
{
	BOOST_TEST((names_of_tags(call("asset.query", R"({"directory":"res://models"})")) == std::vector<std::string>{ "res://models/ship.glb" }));
	BOOST_TEST(names_of_tags(call("asset.query", R"({"directory":"res://"})")).size() == 4u);
}

BOOST_AUTO_TEST_CASE(AssetQuery_SkipsLinksAndNamesThatAreNotUtf8)
{
	std::error_code ec;
	fs::create_symlink(disk.layers[0] / "a.txt", disk.layers[1] / "link.txt", ec);
	const bool file_link = !ec;
	fs::create_directory_symlink(disk.layers[0] / "models", disk.layers[1] / "models_link", ec);
	const bool dir_link = !ec;
	// A file name that is not UTF-8 would make the JSON of the answer invalid; not every file system takes one.
	const std::string bad_name = "bad\xff.txt";
	std::ofstream bad(disk.layers[1] / bad_name, std::ios::binary);
	const bool bad_made = bad.good();
	bad.close();

	const auto result = call("asset.query");
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);

	const auto tags = names_of_tags(result);
	BOOST_TEST((tags == std::vector<std::string>{
		"res://a.txt", "res://models/ship.glb", "res://prefabs/crate.prefab.desc", "res://z.txt" }));
	BOOST_TEST(at(result, "/skipped_names").as_uint64() == (bad_made ? 1u : 0u));
#ifndef _WIN32
	// Not vacuous on the Linux runner: the links and the odd name must have been made for the checks above to mean anything.
	BOOST_REQUIRE(file_link);
	BOOST_REQUIRE(dir_link);
	BOOST_REQUIRE(bad_made);
#endif
	// A directory that is a link is not entered: nothing comes back, nothing outside the layer is read.
	const auto linked = call("asset.query", R"({"directory":"models_link"})");
	BOOST_TEST((!linked.ok || names_of_tags(linked).empty()));
	BOOST_TEST_MESSAGE("links made: file " << file_link << ", directory " << dir_link << ", non-utf8 name " << bad_made);
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
