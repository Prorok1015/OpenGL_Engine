#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_mcp_bridge_host.h"
#include "edt_override_edit.h"
#include "edt_scene_commands.h"
#include "edt_scene_document.h"
#include <snk/command/cmd_registry.h>
#include <snk/desc/desc_node_id.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_selection_service.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <glm/vec3.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The commands that change the scene (A5, #291): every edit one commit under the caller's origin, refs as in
// node.get, override records inside a prefab instance, stale and busy.

namespace {

namespace json = boost::json;
using prefab_node = scn::prefab_desc::prefab_node;

constexpr const char* WORLD = R"({
	"__type": "world_desc", "__version": 3, "name": "w0", "systems": [],
	"children": [
		{ "__id": "00000000000000a2", "name": "MainCamera",
		  "components": { "camera_desc": { "__type": "camera_desc", "fov": 70.0 } } },
		{ "__id": "00000000000000a3", "name": "Crate",
		  "children": [ { "__id": "00000000000000b1", "name": "Lid" } ] },
		{ "__id": "00000000000000a4", "name": "Crate2" }
	]
})";

constexpr const char* PREFAB = R"({
	"__type": "prefab_desc", "__version": 3,
	"children": [
		{ "__id": "00000000000000c1", "name": "eye", "components": { "camera_desc": { "__type": "camera_desc", "fov": 40.0 } } }
	]
})";

constexpr const char* WORLD_1 = R"({
	"__type": "world_desc", "__version": 3, "name": "w1", "systems": [],
	"children": [
		{ "__id": "00000000000000e1", "name": "Inst",
		  "components": { "inst": { "__type": "prefab_desc", "__parent": "memory://t/rig.prefab.desc" } } },
		{ "__id": "00000000000000e2", "name": "Other" },
		{ "__id": "00000000000000e3", "name": "Lens", "components": { "lens": { "__parent": "memory://t/lens.desc" } } }
	]
})";

constexpr const char* LENS = R"({ "__type": "camera_desc", "fov": 30.0 })";

struct commands_fixture : test_util::scene_env
{
	commands_fixture()
	{
		rig = put("rig.prefab.desc", PREFAB);
		put("lens.desc", LENS);
		for (const auto& [name, text] : { std::pair{ "w0", WORLD }, std::pair{ "w1", WORLD_1 } }) {
			auto world = load<scn::world_desc>(put(std::string(name) + ".desc", text));
			BOOST_REQUIRE(world);
			scene.world_descs.push_back(*world);
			scene.world_names.emplace_back(name);
		}
		scene.active_world_idx = 0;
		scene.reset_history();
		scene.mark_clean();
		scene.set_pusher([this] { ++pushes; return true; });
		scene.set_world_reloader([this](std::size_t world) { reloaded.push_back(world); return true; });

		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		registrations = edt::register_scene_commands(commands, { documents, selection, services, [this] { return gesture; } });
	}

	cmd::result call(const std::string& id, const std::string& args, cmd::origin from = cmd::origin::agent("s1"))
	{
		return commands.invoke(id, json::parse(args), from);
	}

	static std::string ref(std::vector<std::string> ids, const std::string& world = "w0")
	{
		std::string path;
		for (const auto& id : ids) path += (path.empty() ? "" : ",") + std::string("\"") + id + "\"";
		return R"({"context":")" + world + R"(","path":[)" + path + "]}";
	}

	prefab_node& root(std::size_t world = 0) { return scene.world_descs[world].get_root(); }

	static const prefab_node* child(const prefab_node& parent, std::uint64_t id)
	{
		for (const auto& node : parent.children) {
			if (node.node_id == id) return &node;
		}
		return nullptr;
	}

	const edt::history_record& last_record() const
	{
		BOOST_REQUIRE(scene.history().next_undo());
		return *scene.history().next_undo();
	}

	res::tag rig;
	edt::editor_services services;
	edt::scene_document scene;
	edt::editor_events events;
	edt::document_manager documents{ scene, services, events };
	edt::selection_service selection{ events };
	cmd::command_registry commands;
	bool gesture = false;
	int pushes = 0;
	std::vector<std::size_t> reloaded;
	std::vector<edt::registration> registrations;
};

const std::vector<const char*> all_commands = { "scene.node.create", "scene.node.delete", "scene.node.duplicate",
	"scene.node.reparent", "scene.node.rename", "component.add", "component.remove", "component.set", "transform.set" };

}

BOOST_FIXTURE_TEST_SUITE(edt_scene_commands_tests, commands_fixture)

BOOST_AUTO_TEST_CASE(the_commands_mutate_and_are_not_offered_over_mcp_yet)
{
	for (const char* id : all_commands) {
		BOOST_TEST_CONTEXT(id) {
			const cmd::command_desc* command = commands.find(id);
			BOOST_REQUIRE(command != nullptr);
			BOOST_TEST(command->mutates);
			BOOST_TEST((command->risk == cmd::danger::none));
			BOOST_TEST(!edt::agent_may_use(*command));
		}
	}
}

BOOST_AUTO_TEST_CASE(create_adds_a_node_the_agent_owns_in_the_history)
{
	const auto before = scene.generation();
	const auto created = call("scene.node.create", R"({"parent":)" + ref({}) + R"(,"name":"Probe",
		"components":{"camera_desc":{"type":"camera_desc","fields":{"fov":60.0}}}})");
	BOOST_REQUIRE_MESSAGE(created.ok, created.error);

	const auto& value = created.value.as_object();
	BOOST_TEST(value.at("generation").as_uint64() == scene.generation());
	BOOST_TEST(scene.generation() == before + 1);
	const auto& path = value.at("ref").as_object().at("path").as_array();
	BOOST_REQUIRE(path.size() == 1u);
	const auto id = desc::parse_node_id(std::string_view(path[0].as_string()));
	BOOST_REQUIRE(id.has_value());
	const prefab_node* probe = child(root(), *id);
	BOOST_REQUIRE(probe != nullptr);
	BOOST_TEST(probe->name == "Probe");
	BOOST_TEST(probe->components.at("camera_desc").overrides.at("fov").to_number<double>() == 60.0);

	BOOST_TEST(last_record().label == "Create node");
	BOOST_TEST((last_record().origin == cmd::origin::agent("s1")));
	BOOST_TEST(pushes == 1);
	BOOST_REQUIRE(scene.undo());
	BOOST_TEST(child(root(), *id) == nullptr);
}

BOOST_AUTO_TEST_CASE(a_refused_edit_changes_nothing)
{
	const auto before = scene.generation();
	const auto unknown = call("scene.node.create", R"({"parent":)" + ref({}) + R"(,
		"components":{"camera_desc":{"type":"camera_desc","fields":{"no_such_field":1}}}})");
	BOOST_TEST(!unknown.ok);
	BOOST_TEST(unknown.code == cmd::codes::invalid_args);
	BOOST_TEST(unknown.error.find("no_such_field") != std::string::npos);

	const auto bad_value = call("component.set", R"({"ref":)" + ref({ "00000000000000a2" }) + R"(,"key":"camera_desc","patch":{"fov":"wide"}})");
	BOOST_TEST(bad_value.code == cmd::codes::invalid_args);

	const auto no_type = call("component.add", R"({"ref":)" + ref({ "00000000000000a4" }) + R"(,"key":"x","type":"no_such_desc"})");
	BOOST_TEST(no_type.code == cmd::codes::invalid_args);

	const auto nowhere = call("scene.node.delete", R"({"ref":)" + ref({ "00000000000000ff" }) + "}");
	BOOST_TEST(nowhere.code == cmd::codes::not_found);

	BOOST_TEST(scene.generation() == before);
	BOOST_TEST(!scene.history().can_undo());
	BOOST_TEST(root().children.size() == 3u);
	BOOST_TEST(root().children[0].components.at("camera_desc").overrides.at("fov").to_number<double>() == 70.0);
}

BOOST_AUTO_TEST_CASE(delete_duplicate_rename_and_reparent)
{
	BOOST_REQUIRE(call("scene.node.rename", R"({"ref":)" + ref({ "00000000000000a4" }) + R"(,"name":"Box"})").ok);
	BOOST_TEST(child(root(), 0xa4)->name == "Box");

	const auto copy = call("scene.node.duplicate", R"({"ref":)" + ref({ "00000000000000a3" }) + "}");
	BOOST_REQUIRE_MESSAGE(copy.ok, copy.error);
	BOOST_REQUIRE(root().children.size() == 4u);
	const prefab_node& duplicate = root().children[2];
	BOOST_TEST(duplicate.name == "Crate_copy");
	BOOST_TEST(duplicate.node_id != 0xa3u);
	BOOST_REQUIRE(duplicate.children.size() == 1u);
	BOOST_TEST(duplicate.children[0].node_id != 0xb1u);

	const auto moved = call("scene.node.reparent", R"({"ref":)" + ref({ "00000000000000a4" }) + R"(,"parent":)" + ref({ "00000000000000a3" }) + R"(,"index":0})");
	BOOST_REQUIRE_MESSAGE(moved.ok, moved.error);
	BOOST_TEST(child(root(), 0xa4) == nullptr);
	BOOST_REQUIRE(child(root(), 0xa3)->children.size() == 2u);
	BOOST_TEST(child(root(), 0xa3)->children[0].node_id == 0xa4u);
	BOOST_TEST(json::serialize(moved.value.at("ref")) == R"({"context":"w0","path":["00000000000000a3","00000000000000a4"]})");

	const auto under_itself = call("scene.node.reparent", R"({"ref":)" + ref({ "00000000000000a3" }) + R"(,"parent":)" + ref({ "00000000000000a3", "00000000000000b1" }) + "}");
	BOOST_TEST(under_itself.code == cmd::codes::invalid_args);
	const auto root_delete = call("scene.node.delete", R"({"ref":)" + ref({}) + "}");
	BOOST_TEST(root_delete.code == cmd::codes::invalid_args);

	BOOST_REQUIRE(call("scene.node.delete", R"({"ref":)" + ref({ "00000000000000a2" }) + "}").ok);
	BOOST_TEST(child(root(), 0xa2) == nullptr);

	// Four edits, four records: undo walks them back.
	for (int i = 0; i < 4; ++i) BOOST_REQUIRE(scene.undo());
	BOOST_TEST(!scene.history().can_undo());
	BOOST_TEST(root().children.size() == 3u);
	BOOST_TEST(child(root(), 0xa4)->name == "Crate2");
}

BOOST_AUTO_TEST_CASE(component_edits_of_a_node_of_the_scene)
{
	const std::string crate = ref({ "00000000000000a4" });
	BOOST_REQUIRE(call("component.add", R"({"ref":)" + crate + R"(,"key":"cam","type":"camera_desc","fields":{"fov":30.0}})").ok);
	BOOST_TEST(child(root(), 0xa4)->components.at("cam").type_name == "camera_desc");
	const auto twice = call("component.add", R"({"ref":)" + crate + R"(,"key":"cam","type":"camera_desc"})");
	BOOST_TEST(twice.code == cmd::codes::invalid_args);

	BOOST_REQUIRE(call("component.set", R"({"ref":)" + crate + R"(,"key":"cam","patch":{"fov":45.0}})").ok);
	BOOST_TEST(child(root(), 0xa4)->components.at("cam").overrides.at("fov").to_number<double>() == 45.0);
	BOOST_REQUIRE(call("component.set", R"({"ref":)" + crate + R"(,"key":"cam","patch":{"fov":null}})").ok);
	BOOST_TEST(!child(root(), 0xa4)->components.at("cam").overrides.contains("fov"));

	BOOST_REQUIRE(call("component.remove", R"({"ref":)" + crate + R"(,"key":"cam"})").ok);
	BOOST_TEST(!child(root(), 0xa4)->components.contains("cam"));
	BOOST_TEST(call("component.remove", R"({"ref":)" + crate + R"(,"key":"cam"})").code == cmd::codes::not_found);

	BOOST_REQUIRE(call("transform.set", R"({"ref":)" + crate + R"(,"position":[1,2,3]})").ok);
	BOOST_TEST(child(root(), 0xa4)->position.y == 2.0f);
	BOOST_TEST(child(root(), 0xa4)->has_transform);
	BOOST_TEST(call("transform.set", R"({"ref":)" + crate + R"(,"position":[1,2]})").code == cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(inside_a_prefab_instance_only_overrides)
{
	const std::string eye = ref({ "00000000000000e1", "00000000000000c1" }, "w1");
	BOOST_TEST(call("scene.node.create", R"({"parent":)" + eye + "}").code == cmd::codes::failed);
	BOOST_TEST(call("scene.node.delete", R"({"ref":)" + eye + "}").code == cmd::codes::failed);
	BOOST_TEST(call("component.add", R"({"ref":)" + eye + R"(,"key":"x","type":"camera_desc"})").code == cmd::codes::failed);
	// A path below a plain node leads nowhere.
	BOOST_TEST(call("scene.node.rename", R"({"ref":)" + ref({ "00000000000000e2", "00000000000000c1" }, "w1") + R"(,"name":"x"})").code == cmd::codes::not_found);

	const auto set = call("component.set", R"({"ref":)" + eye + R"(,"key":"camera_desc","patch":{"fov":55.0}})");
	BOOST_REQUIRE_MESSAGE(set.ok, set.error);
	const prefab_node& inst = *child(root(1), 0xe1);
	BOOST_REQUIRE(inst.overrides.size() == 1u);
	BOOST_TEST((inst.overrides[0].target == std::vector<std::uint64_t>{ 0xc1 }));
	BOOST_TEST(inst.overrides[0].component == "camera_desc");

	const auto moved = call("transform.set", R"({"ref":)" + eye + R"(,"position":[0,5,0]})");
	BOOST_REQUIRE_MESSAGE(moved.ok, moved.error);
	BOOST_TEST(inst.overrides.size() > 1u);
	BOOST_TEST(scene.history().size() == 2u);
}

BOOST_AUTO_TEST_CASE(an_edit_built_on_an_old_generation_is_stale)
{
	const auto now = scene.generation();
	const auto stale = call("scene.node.rename", R"({"ref":)" + ref({ "00000000000000a4" }) + R"(,"name":"Box","expected_generation":)" + std::to_string(now + 7) + "}");
	BOOST_TEST(stale.code == cmd::codes::stale);
	BOOST_TEST(stale.value.at("generation").as_uint64() == now);
	BOOST_TEST(child(root(), 0xa4)->name == "Crate2");

	BOOST_REQUIRE(call("scene.node.rename", R"({"ref":)" + ref({ "00000000000000a4" }) + R"(,"name":"Box","expected_generation":)" + std::to_string(now) + "}").ok);

	cmd::call request;
	request.args = json::parse(R"({"ref":)" + ref({ "00000000000000a4" }) + R"(,"name":"Bin"})");
	request.from = cmd::origin::agent("s1");
	request.expected_generation = now;
	BOOST_TEST(commands.invoke("scene.node.rename", request).code == cmd::codes::stale);
}

BOOST_AUTO_TEST_CASE(an_agent_waits_for_the_persons_gesture)
{
	gesture = true;
	const std::string args = R"({"ref":)" + ref({ "00000000000000a4" }) + R"(,"name":"Box"})";
	BOOST_TEST(call("scene.node.rename", args).code == cmd::codes::busy);
	BOOST_TEST(child(root(), 0xa4)->name == "Crate2");
	// The person's own scripts are not held back by their gesture.
	BOOST_TEST(call("scene.node.rename", args, cmd::origin::ui()).ok);
	BOOST_TEST((last_record().origin == cmd::origin::ui()));
}

BOOST_AUTO_TEST_CASE(an_edit_of_another_world_reloads_that_world_only)
{
	BOOST_REQUIRE(call("scene.node.rename", R"({"ref":)" + ref({ "00000000000000e2" }, "w1") + R"(,"name":"Renamed"})").ok);
	BOOST_TEST((reloaded == std::vector<std::size_t>{ 1 }), boost::test_tools::per_element());
	BOOST_TEST(pushes == 0);

	BOOST_REQUIRE(scene.undo());
	BOOST_TEST(child(root(1), 0xe2)->name == "Other");
	BOOST_REQUIRE(scene.redo());
	BOOST_TEST(child(root(1), 0xe2)->name == "Renamed");
	BOOST_TEST((reloaded == std::vector<std::size_t>{ 1, 1, 1 }), boost::test_tools::per_element());
	BOOST_TEST(pushes == 0);
}

BOOST_AUTO_TEST_CASE(an_edit_that_changes_nothing_commits_nothing)
{
	const auto before = scene.generation();
	const auto renamed = call("scene.node.rename", R"({"ref":)" + ref({ "00000000000000a4" }) + R"(,"name":"Crate2"})");
	BOOST_REQUIRE(renamed.ok);
	BOOST_TEST(renamed.value.as_object().at("unchanged").as_bool());
	BOOST_TEST(scene.generation() == before);
	BOOST_TEST(pushes == 0);
	BOOST_TEST(!scene.history().can_undo());
}

BOOST_AUTO_TEST_CASE(an_edit_clears_the_selection)
{
	edt::selection picked;
	picked.world = "w0";
	picked.path = { 0xa4 };
	selection.select(picked);
	BOOST_REQUIRE(selection.has_selection());

	BOOST_REQUIRE(call("scene.node.delete", R"({"ref":)" + ref({ "00000000000000a4" }) + "}").ok);
	BOOST_TEST(!selection.has_selection());
}

BOOST_AUTO_TEST_CASE(a_component_that_only_references_a_desc_has_that_desc_type)
{
	const std::string lens = ref({ "00000000000000e3" }, "w1");
	const auto set = call("component.set", R"({"ref":)" + lens + R"(,"key":"lens","patch":{"fov":33.0}})");
	BOOST_REQUIRE_MESSAGE(set.ok, set.error);
	BOOST_TEST(child(root(1), 0xe3)->components.at("lens").overrides.at("fov").to_number<double>() == 33.0);

	const auto unknown = call("component.set", R"({"ref":)" + lens + R"(,"key":"lens","patch":{"no_such_field":1}})");
	BOOST_TEST(unknown.code == cmd::codes::invalid_args);
	BOOST_TEST(unknown.error.find("camera_desc") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_edit_to_how_things_are_says_unchanged_and_keeps_the_generation)
{
	const std::string crate = ref({ "00000000000000a4" });
	BOOST_REQUIRE(call("transform.set", R"({"ref":)" + crate + R"(,"position":[1,2,3]})").ok);
	const auto generation = scene.generation();
	const auto records = scene.history().size();

	for (const std::string& args : {
			R"({"ref":)" + crate + R"(,"position":[1,2,3]})",
			R"({"ref":)" + crate + R"(,"name":"Crate2"})",
			R"({"ref":)" + crate + R"(,"parent":)" + ref({}) + R"(,"index":2})",
			R"({"ref":)" + crate + R"(,"parent":)" + ref({}) + "}",
			R"({"ref":)" + crate + R"(,"parent":)" + ref({}) + R"(,"index":99})" }) {
		const std::string id = args.find("position") != std::string::npos ? "transform.set"
			: args.find("name") != std::string::npos ? "scene.node.rename" : "scene.node.reparent";
		BOOST_TEST_CONTEXT(id << " " << args) {
			const auto outcome = call(id, args);
			BOOST_REQUIRE_MESSAGE(outcome.ok, outcome.error);
			BOOST_TEST(outcome.value.as_object().at("unchanged").as_bool());
			BOOST_TEST(scene.generation() == generation);
		}
	}
	BOOST_TEST(scene.history().size() == records);
}

BOOST_AUTO_TEST_CASE(a_commit_that_changed_no_desc_still_rebuilds_the_active_world)
{
	// A gizmo drag that an override rounds back to the base: the desc is as it was, the live entity is not.
	scene.commit("Move entity");
	BOOST_TEST(pushes == 1);
	BOOST_TEST(!scene.history().can_undo());
}

BOOST_AUTO_TEST_CASE(arguments_that_do_not_fit_are_refused)
{
	const std::string crate = ref({ "00000000000000a4" });
	BOOST_TEST(call("transform.set", R"({"ref":)" + crate + R"(,"scale":[1,1e39,1]})").code == cmd::codes::invalid_args);
	BOOST_TEST(call("scene.node.rename", R"({"ref":)" + crate + R"(,"name":"a\u0001b"})").code == cmd::codes::invalid_args);
	BOOST_TEST(call("scene.node.rename", R"({"ref":)" + crate + R"(,"name":")" + std::string(257, 'x') + R"("})").code == cmd::codes::invalid_args);
	BOOST_TEST(call("scene.node.rename", R"({"ref":)" + crate + R"(,"name":")" + std::string(256, 'x') + R"("})").ok);
	BOOST_TEST(call("component.add", R"({"ref":)" + crate + R"(,"key":"__type","type":"camera_desc"})").code == cmd::codes::invalid_args);
	BOOST_TEST(call("scene.node.create", R"({"parent":)" + ref({}) + R"(,"components":{"__parent":{"type":"camera_desc"}}})").code == cmd::codes::invalid_args);
	BOOST_TEST(call("scene.node.create", R"({"parent":)" + ref({}) + R"(,"components":{"":{"type":"camera_desc"}}})").code == cmd::codes::invalid_args);

	// 0: not given, so the generation of the call still counts.
	cmd::call request;
	request.args = json::parse(R"({"ref":)" + crate + R"(,"name":"Bin","expected_generation":0})");
	request.from = cmd::origin::agent("s1");
	request.expected_generation = scene.generation() + 3;
	BOOST_TEST(commands.invoke("scene.node.rename", request).code == cmd::codes::stale);
}

BOOST_AUTO_TEST_CASE(a_create_with_one_bad_component_creates_nothing)
{
	const auto created = call("scene.node.create", R"({"parent":)" + ref({}) + R"(,"components":{
		"good":{"type":"camera_desc","fields":{"fov":50.0}},
		"bad":{"type":"camera_desc","fields":{"no_such_field":1}}}})");
	BOOST_TEST(created.code == cmd::codes::invalid_args);
	BOOST_TEST(root().children.size() == 3u);
	BOOST_TEST(!scene.history().can_undo());
}

BOOST_AUTO_TEST_CASE(copies_get_free_names_and_moves_stay_in_their_world)
{
	const std::string crate = ref({ "00000000000000a3" });
	BOOST_REQUIRE(call("scene.node.duplicate", R"({"ref":)" + crate + "}").ok);
	BOOST_REQUIRE(call("scene.node.duplicate", R"({"ref":)" + crate + "}").ok);
	std::vector<std::string> names;
	for (const auto& node : root().children) names.push_back(node.name);
	BOOST_TEST((names == std::vector<std::string>{ "MainCamera", "Crate", "Crate_copy_1", "Crate_copy", "Crate2" }), boost::test_tools::per_element());

	BOOST_TEST(call("scene.node.reparent", R"({"ref":)" + crate + R"(,"parent":)" + ref({}, "w1") + "}").code == cmd::codes::invalid_args);
	BOOST_TEST(call("scene.node.reparent", R"({"ref":)" + ref({}) + R"(,"parent":)" + crate + "}").code == cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(a_copy_of_an_instance_keeps_its_overrides_on_the_prefab)
{
	const std::string eye = ref({ "00000000000000e1", "00000000000000c1" }, "w1");
	BOOST_REQUIRE(call("component.set", R"({"ref":)" + eye + R"(,"key":"camera_desc","patch":{"fov":55.0}})").ok);

	const auto copy = call("scene.node.duplicate", R"({"ref":)" + ref({ "00000000000000e1" }, "w1") + "}");
	BOOST_REQUIRE_MESSAGE(copy.ok, copy.error);
	const prefab_node& original = *child(root(1), 0xe1);
	const prefab_node& duplicate = root(1).children[1];
	BOOST_TEST(duplicate.node_id != original.node_id);
	BOOST_TEST(duplicate.name == "Inst_copy");
	// The records address the prefab's content, which keeps its ids.
	BOOST_REQUIRE(duplicate.overrides.size() == original.overrides.size());
	BOOST_TEST((duplicate.overrides[0].target == std::vector<std::uint64_t>{ 0xc1 }));
}

BOOST_AUTO_TEST_CASE(values_written_inside_an_instance_and_back)
{
	const std::string eye = ref({ "00000000000000e1", "00000000000000c1" }, "w1");
	BOOST_REQUIRE(call("component.set", R"({"ref":)" + eye + R"(,"key":"camera_desc","patch":{"fov":55.0}})").ok);
	BOOST_REQUIRE(call("transform.set", R"({"ref":)" + eye + R"(,"position":[0,5,0]})").ok);

	const prefab_node& inst = *child(root(1), 0xe1);
	const auto base = edt::override_edit::find_base_node(*assembler, inst, std::vector<std::uint64_t>{ 0xc1 });
	BOOST_REQUIRE(base.has_value());
	const auto effective = edt::override_edit::effective_component(inst, std::vector<std::uint64_t>{ 0xc1 }, "camera_desc",
		edt::override_edit::component_json(base->components.at("camera_desc")));
	BOOST_TEST(effective.at("fov").to_number<double>() == 55.0);
	glm::vec3 position{}, rotation{}, scale{ 1.0f };
	edt::override_edit::effective_transform(inst, std::vector<std::uint64_t>{ 0xc1 }, *base, position, rotation, scale);
	BOOST_TEST(position.y == 5.0f);
	BOOST_TEST(std::ranges::any_of(inst.overrides, [](const scn::override_record& r) { return r.component == scn::transform_pseudo_component; }));

	BOOST_REQUIRE(call("component.set", R"({"ref":)" + eye + R"(,"key":"camera_desc","patch":{"fov":null}})").ok);
	BOOST_TEST(std::ranges::none_of(inst.overrides, [](const scn::override_record& r) { return r.component == "camera_desc"; }));

	// The prefab itself is not touched.
	auto prefab = load<scn::prefab_desc>(rig);
	BOOST_REQUIRE(prefab);
	BOOST_TEST(prefab->get_root().children[0].components.at("camera_desc").overrides.at("fov").to_number<double>() == 40.0);
}

BOOST_AUTO_TEST_CASE(a_script_is_not_held_back_by_a_gesture)
{
	gesture = true;
	BOOST_TEST(call("scene.node.rename", R"({"ref":)" + ref({ "00000000000000a4" }) + R"(,"name":"Box"})", cmd::origin::test()).ok);
}

BOOST_AUTO_TEST_CASE(a_copy_of_a_long_name_stays_a_valid_name)
{
	const std::string crate = ref({ "00000000000000a3" });
	// 255 bytes of two-byte characters and one 'x': the cut must not split a character.
	std::string json_name = "x";
	for (int i = 0; i < 127; ++i) json_name += "\\u044f";
	BOOST_REQUIRE(call("scene.node.rename", R"({"ref":)" + crate + R"(,"name":")" + json_name + R"("})").ok);
	BOOST_REQUIRE(child(root(), 0xa3)->name.size() == 255u);

	BOOST_REQUIRE(call("scene.node.duplicate", R"({"ref":)" + crate + "}").ok);
	BOOST_REQUIRE(call("scene.node.duplicate", R"({"ref":)" + crate + "}").ok);
	for (const auto& node : root().children) {
		BOOST_TEST_CONTEXT(node.name.size()) {
			BOOST_TEST(node.name.size() <= 256u);
			BOOST_TEST(!node.name.empty());
			// Whole characters only: no name ends inside a two-byte sequence.
			const std::size_t before_ascii = node.name.find_last_not_of("_copy0123456789");
			if (node.name.size() > 200 && before_ascii != std::string::npos) {
				BOOST_TEST((static_cast<unsigned char>(node.name[before_ascii]) & 0xC0) == 0x80u);
			}
		}
	}
	BOOST_TEST(root().children[2].name.ends_with("_copy_1"));
	BOOST_TEST(root().children[3].name.ends_with("_copy"));
}

BOOST_AUTO_TEST_CASE(a_utf8_name_is_a_name)
{
	BOOST_REQUIRE(call("scene.node.rename", R"({"ref":)" + ref({ "00000000000000a4" }) + R"(,"name":"Ящик"})").ok);
	BOOST_TEST(child(root(), 0xa4)->name == std::string(json::parse(R"("Ящик")").as_string()));
}

BOOST_AUTO_TEST_CASE(the_same_index_under_another_parent_is_a_move)
{
	const auto moved = call("scene.node.reparent", R"({"ref":)" + ref({ "00000000000000a3", "00000000000000b1" }) + R"(,"parent":)" + ref({}) + R"(,"index":0})");
	BOOST_REQUIRE_MESSAGE(moved.ok, moved.error);
	BOOST_TEST(!moved.value.as_object().contains("unchanged"));
	BOOST_TEST(root().children[0].node_id == 0xb1u);
	BOOST_TEST(child(root(), 0xa3)->children.empty());
}

BOOST_AUTO_TEST_SUITE_END()
