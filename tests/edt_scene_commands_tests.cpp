#include "edt_prefab_preview_kinds.hpp"
#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_agent_policy.h"
#include "edt_override_edit.h"
#include "edt_scene_commands.h"
#include "edt_scene_document.h"
#include "edt_document_cast.h"
#include <snk/command/cmd_registry.h>
#include <snk/desc/desc_describe.h>
#include <snk/desc/desc_describe_ops.h>
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_node_id.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_schema_desc.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_selection_service.h>
#include <snk/resource/res_asset_index.h>
#include <snk/resource/res_system.h>
#include <boost/describe.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <glm/vec3.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The commands that change the scene (A5, #291): every edit one commit under the caller's origin, refs as in
// node.get, override records inside a prefab instance, stale and busy.

namespace {

namespace json = boost::json;
using prefab_node = scn::prefab_desc::prefab_node;

constexpr const char* WORLD = R"({
	"__type": "world_desc", "__version": 4, "name": "w0", "systems": [],
	"children": [
		{ "__id": "00000000000000a2", "name": "MainCamera",
		  "components": { "camera_desc": { "__type": "camera_desc", "__version": 2, "fov": 70.0 } } },
		{ "__id": "00000000000000a3", "name": "Crate",
		  "children": [ { "__id": "00000000000000b1", "name": "Lid" } ] },
		{ "__id": "00000000000000a4", "name": "Crate2" }
	]
})";

constexpr const char* PREFAB = R"({
	"__type": "prefab_desc", "__version": 4,
	"children": [
		{ "__id": "00000000000000c1", "name": "eye", "components": {
			"camera_desc": { "__type": "camera_desc", "__version": 2, "fov": 40.0 },
			"material": { "__type": "material_desc", "__version": 1 },
			"mesh": { "__type": "mesh_node_desc", "vx_end": 3, "ind_end": 3 } } }
	]
})";

constexpr const char* WORLD_1 = R"({
	"__type": "world_desc", "__version": 4, "name": "w1", "systems": [],
	"children": [
		{ "__id": "00000000000000e1", "name": "Inst",
		  "components": { "inst": { "__type": "prefab_desc", "__parent": "memory://t/rig.prefab.desc" } } },
		{ "__id": "00000000000000e2", "name": "Other" },
		{ "__id": "00000000000000e3", "name": "Lens", "components": { "lens": "memory://t/lens.desc" } }
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
	edt::document_manager documents{ scene, events };
	test_util::prefab_preview_kinds prefab_kinds{ documents, services };
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

namespace {

// A map of structures that hold a tag: a {guid, path} reference inside one of its values replaces what is there (#773).
struct icon_slot
{
	res::tag icon;
};
BOOST_DESCRIBE_STRUCT(icon_slot, (), (icon))

class slot_map_component : public desc::schema_desc<slot_map_component, desc::schema_kind::component>
{
public:
	static constexpr std::string_view __type = "probe.slot_map";

	std::map<std::string, icon_slot> slots;

	BOOST_DESCRIBE_CLASS(slot_map_component, (), (slots), (), ())
};

}

namespace desc
{
	template<>
	inline constexpr auto field_attrs<slot_map_component> = attrs(f("slots").ref("probe.icon_slot"));
}

BOOST_FIXTURE_TEST_SUITE(edt_scene_commands_tests, commands_fixture)

BOOST_AUTO_TEST_CASE(the_commands_mutate_and_the_owners_table_allows_them_to_an_agent)
{
	for (const char* id : all_commands) {
		BOOST_TEST_CONTEXT(id) {
			const cmd::command_desc* command = commands.find(id);
			BOOST_REQUIRE(command != nullptr);
			BOOST_TEST(command->mutates);
			BOOST_TEST((command->risk == cmd::danger::none));
			BOOST_TEST((command->category == cmd::agent_category::scene_edit));
			BOOST_TEST((edt::default_mode(*command) == cmd::policy_mode::allow));
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

// The nested fields of a schema are material_desc's /uniforms/albedo, /uniforms/emissive and /uniforms/shininess.
namespace {
	constexpr const char* add_material = R"(,"key":"mat","type":"material_desc"})";
}

BOOST_AUTO_TEST_CASE(a_nested_patch_of_the_schema_leaves_is_applied_and_undone)
{
	const std::string crate = ref({ "00000000000000a4" });
	BOOST_REQUIRE(call("component.add", R"({"ref":)" + crate + add_material).ok);
	const auto set = call("component.set", R"({"ref":)" + crate
		+ R"(,"key":"mat","patch":{"uniforms":{"shininess":32,"albedo":[1,0,0,1]}}})");
	BOOST_REQUIRE_MESSAGE(set.ok, set.error);
	const auto& overrides = child(root(), 0xa4)->components.at("mat").overrides;
	const auto& uniforms = overrides.at("uniforms").as_object();
	BOOST_TEST(uniforms.at("shininess").to_number<double>() == 32.0);
	BOOST_REQUIRE(uniforms.at("albedo").as_array().size() == 4u);
	BOOST_TEST(uniforms.at("albedo").as_array()[0].to_number<double>() == 1.0);

	BOOST_REQUIRE(scene.undo()); // the nested patch was one step
	const auto& undone = child(root(), 0xa4)->components.at("mat").overrides;
	BOOST_TEST(!undone.contains("uniforms"));
	BOOST_REQUIRE(scene.undo()); // and the add before it another
	BOOST_TEST(!scene.history().can_undo());
}

BOOST_AUTO_TEST_CASE(a_nested_patch_inside_a_prefab_instance_becomes_override_records)
{
	const std::string eye = ref({ "00000000000000e1", "00000000000000c1" }, "w1");
	const auto set = call("component.set", R"({"ref":)" + eye
		+ R"(,"key":"material","patch":{"uniforms":{"shininess":64}}})");
	BOOST_REQUIRE_MESSAGE(set.ok, set.error);
	const prefab_node& inst = *child(root(1), 0xe1);
	// the editor writes a diff of the component as the value of one record at ""
	BOOST_TEST(std::ranges::any_of(inst.overrides, [](const scn::override_record& r) {
		return r.component == "material" && r.path.empty() && r.value.is_object()
			&& r.value.as_object().contains("uniforms") && r.value.as_object().at("uniforms").as_object().contains("shininess"); }));

	BOOST_TEST(call("component.set", R"({"ref":)" + eye
		+ R"(,"key":"material","patch":{"uniforms":{"gloss":1}}})").code == cmd::codes::invalid_args);
	BOOST_REQUIRE(scene.undo());
	BOOST_TEST(!scene.history().can_undo());
}

BOOST_AUTO_TEST_CASE(a_patch_that_changes_nothing_commits_nothing_and_leaves_no_empty_object)
{
	const std::string crate = ref({ "00000000000000a4" });
	BOOST_REQUIRE(call("component.add", R"({"ref":)" + crate + add_material).ok);
	const auto before = scene.generation();
	const auto steps = scene.history().size();
	for (const char* patch : { R"({"uniforms":{}})", R"({"uniforms":{"shininess":null}})", R"({"uniforms":{"shininess":null,"albedo":null}})" }) {
		BOOST_TEST_INFO(patch);
		const auto result = call("component.set", R"({"ref":)" + crate + R"(,"key":"mat","patch":)" + patch + "}");
		BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	}
	BOOST_TEST(scene.generation() == before);
	BOOST_TEST(scene.history().size() == steps);
	BOOST_TEST(!child(root(), 0xa4)->components.at("mat").overrides.contains("uniforms"));

	// A nested object the patch empties by nulls goes.
	BOOST_REQUIRE(call("component.set", R"({"ref":)" + crate + R"(,"key":"mat","patch":{"uniforms":{"shininess":5}}})").ok);
	BOOST_REQUIRE(call("component.set", R"({"ref":)" + crate + R"(,"key":"mat","patch":{"uniforms":{"shininess":null}}})").ok);
	BOOST_TEST(!child(root(), 0xa4)->components.at("mat").overrides.contains("uniforms"));
}

BOOST_AUTO_TEST_CASE(a_nested_field_is_refused_with_its_whole_path)
{
	const std::string crate = ref({ "00000000000000a4" });
	BOOST_REQUIRE(call("component.add", R"({"ref":)" + crate + add_material).ok);
	const auto before = scene.generation();
	const auto steps = scene.history().size();
	const auto unknown = call("component.set", R"({"ref":)" + crate
		+ R"(,"key":"mat","patch":{"uniforms":{"shininess":10,"glow":[1,1,1,1]}}})");
	BOOST_TEST(unknown.code == cmd::codes::invalid_args);
	BOOST_TEST(unknown.error.find("uniforms/glow (no such field)") != std::string::npos);

	const auto not_fitting = call("component.set", R"({"ref":)" + crate
		+ R"(,"key":"mat","patch":{"uniforms":{"shininess":"hard"}}})");
	BOOST_TEST(not_fitting.code == cmd::codes::invalid_args);
	BOOST_TEST(not_fitting.error.find("uniforms/shininess (the value does not fit)") != std::string::npos);

	const auto no_container = call("component.set", R"({"ref":)" + crate + R"(,"key":"mat","patch":{"uniforms":{"shininess":{"x":1}}}})");
	BOOST_TEST(no_container.code == cmd::codes::invalid_args);

	BOOST_TEST(scene.generation() == before);
	BOOST_TEST(scene.history().size() == steps);
}

BOOST_AUTO_TEST_CASE(a_new_component_may_carry_nested_fields)
{
	const std::string crate = ref({ "00000000000000a4" });
	const auto added = call("component.add", R"({"ref":)" + crate
		+ R"(,"key":"mat","type":"material_desc","fields":{"uniforms":{"shininess":10,"albedo":[0,1,0,1]}}})");
	BOOST_REQUIRE_MESSAGE(added.ok, added.error);
	BOOST_TEST(child(root(), 0xa4)->components.at("mat").overrides.at("uniforms").as_object().at("albedo").as_array().size() == 4u);
	BOOST_TEST(call("component.add", R"({"ref":)" + crate
		+ R"(,"key":"mat2","type":"material_desc","fields":{"uniforms":{"nope":1}}})").code == cmd::codes::invalid_args);
}

BOOST_AUTO_TEST_CASE(a_new_component_keeps_no_nulls)
{
	const std::string crate = ref({ "00000000000000a4" });
	const auto added = call("component.add", R"({"ref":)" + crate
		+ R"(,"key":"mat","type":"material_desc","fields":{"queue":"transparent","render_mode":null,"uniforms":{"shininess":null}}})");
	BOOST_REQUIRE_MESSAGE(added.ok, added.error);
	const auto& overrides = child(root(), 0xa4)->components.at("mat").overrides;
	BOOST_TEST(overrides.at("queue").as_string() == "transparent");
	BOOST_TEST(!overrides.contains("render_mode"));
	BOOST_TEST(!overrides.contains("uniforms"));
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
	// "key": "<tag>" is the component with no type of its own (in the object form the key is the type).
	BOOST_REQUIRE(child(root(1), 0xe3)->components.at("lens").type_name.empty());
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

// #720: mesh_node_desc has a schema, so an agent hides a mesh with component.set like any other field, on a node of the
// scene and inside a prefab instance.
BOOST_AUTO_TEST_CASE(an_agent_hides_a_mesh_with_component_set)
{
	const std::string crate = ref({ "00000000000000a4" });
	BOOST_REQUIRE(call("component.add", R"({"ref":)" + crate
		+ R"(,"key":"mesh","type":"mesh_node_desc","fields":{"vx_end":3,"ind_end":3}})").ok);
	const auto hide = call("component.set", R"({"ref":)" + crate + R"(,"key":"mesh","patch":{"visible":false}})");
	BOOST_REQUIRE_MESSAGE(hide.ok, hide.error);
	BOOST_TEST(child(root(), 0xa4)->components.at("mesh").overrides.at("visible").as_bool() == false);
	BOOST_TEST(call("component.set", R"({"ref":)" + crate + R"(,"key":"mesh","patch":{"visible":"no"}})").code
		== cmd::codes::invalid_args);
	BOOST_TEST(call("component.set", R"({"ref":)" + crate + R"(,"key":"mesh","patch":{"colour":1}})").code
		== cmd::codes::invalid_args);

	const std::string eye = ref({ "00000000000000e1", "00000000000000c1" }, "w1");
	const auto inside = call("component.set", R"({"ref":)" + eye + R"(,"key":"mesh","patch":{"visible":false}})");
	BOOST_REQUIRE_MESSAGE(inside.ok, inside.error);
	const prefab_node& inst = *child(root(1), 0xe1);
	BOOST_TEST(std::ranges::any_of(inst.overrides, [](const scn::override_record& r) {
		return r.component == "mesh" && r.value.is_object() && r.value.as_object().contains("visible"); }));
}

// #697 at component.set (#720): a tag field of any type with a schema may name res://, engine:// or memory://, never a
// protocol of the player's, the editor's or the project's own files; the shape alone used to pass.
BOOST_AUTO_TEST_CASE(a_tag_field_refuses_a_reference_a_desc_may_not_hold)
{
	const std::string crate = ref({ "00000000000000a4" });
	BOOST_REQUIRE(call("component.add", R"({"ref":)" + crate
		+ R"(,"key":"mesh","type":"mesh_node_desc","fields":{"vx_end":3,"ind_end":3}})").ok);
	for (const char* bad : { "user://saves/x.desc", "cache://x.desc", "edt://x.desc", "code://x.desc" }) {
		BOOST_TEST_CONTEXT(bad) {
			const auto geometry = call("component.set", R"({"ref":)" + crate + R"(,"key":"mesh","patch":{"geometry":")" + bad + R"("}})");
			BOOST_TEST(geometry.code == cmd::codes::invalid_args);
			BOOST_TEST(geometry.error.find("geometry") != std::string::npos);
			BOOST_TEST(call("component.set", R"({"ref":)" + crate + R"(,"key":"mesh","patch":{"material":")" + bad + R"("}})").code
				== cmd::codes::invalid_args);
		}
	}
	BOOST_TEST(call("component.set", R"({"ref":)" + crate + R"(,"key":"mesh","patch":{"material":"no_protocol.desc"}})").code
		== cmd::codes::invalid_args);
	BOOST_TEST(!child(root(), 0xa4)->components.at("mesh").overrides.contains("geometry"));

	// What a desc may hold passes: whether the file is there is the pre-Play check's to say.
	const auto fine = call("component.set", R"({"ref":)" + crate
		+ R"(,"key":"mesh","patch":{"geometry":"res://not/there.geom.desc","material":"engine://materials/default.mat.desc"}})");
	BOOST_REQUIRE_MESSAGE(fine.ok, fine.error);

	// The rule is the same for every type with a schema: the sky's material, a component added with the field.
	BOOST_TEST(call("component.add", R"({"ref":)" + crate
		+ R"(,"key":"sky","type":"skybox_desc","fields":{"material":"user://sky.mat.desc"}})").code == cmd::codes::invalid_args);
	BOOST_TEST(!child(root(), 0xa4)->components.contains("sky"));
}

// Nested references follow #697 too: a tag inside a structure and inside an array of structures, named by its full path.
BOOST_AUTO_TEST_CASE(a_tag_inside_a_structure_or_an_array_of_them_is_checked)
{
	BOOST_REQUIRE(descs->register_schema(desc::parse_type_schema(json::parse(R"({"name":"probe.slot","kind":"struct",
		"fields":[{"name":"icon","kind":"res_tag","default":""}]})").as_object())));
	BOOST_REQUIRE(descs->register_schema(desc::parse_type_schema(json::parse(R"({"name":"probe.refs","kind":"component",
		"fields":[{"name":"slot","kind":"structure","ref_type":"probe.slot"},
			{"name":"slots","kind":"array","element":"structure","ref_type":"probe.slot"}]})").as_object(), &descs->schemas())));
	const std::string crate = ref({ "00000000000000a4" });
	const auto added = call("component.add", R"({"ref":)" + crate + R"(,"key":"refs","type":"probe.refs"})");
	BOOST_REQUIRE_MESSAGE(added.ok, added.error);

	const auto in_structure = call("component.set", R"({"ref":)" + crate + R"(,"key":"refs","patch":{"slot":{"icon":"user://x.png"}}})");
	BOOST_TEST(in_structure.code == cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(in_structure.error.find("slot/icon") != std::string::npos, in_structure.error);

	const auto in_array = call("component.set", R"({"ref":)" + crate
		+ R"(,"key":"refs","patch":{"slots":[{"icon":"res://a.png"},{"icon":"cache://b.png"}]}})");
	BOOST_TEST(in_array.code == cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(in_array.error.find("slots/1/icon") != std::string::npos, in_array.error);
	BOOST_TEST(!child(root(), 0xa4)->components.at("refs").overrides.contains("slots"));

	const auto fine = call("component.set", R"({"ref":)" + crate
		+ R"(,"key":"refs","patch":{"slot":{"icon":"res://ok.png"},"slots":[{"icon":"engine://ok.png"}]}})");
	BOOST_CHECK_MESSAGE(fine.ok, fine.error);
}

namespace {

// A component with a map of tags: every value is a reference, so each one follows #697.
class tag_map_component : public desc::schema_desc<tag_map_component, desc::schema_kind::component>
{
public:
	static constexpr std::string_view __type = "probe.tag_map";

	std::map<std::string, res::tag> icons;

	BOOST_DESCRIBE_CLASS(tag_map_component, (), (icons), (), ())
};

}

BOOST_AUTO_TEST_CASE(a_tag_inside_a_map_is_checked_by_its_key)
{
	descs->register_component_desc<tag_map_component>("probe.tag_map");
	const std::string crate = ref({ "00000000000000a4" });
	const auto added = call("component.add", R"({"ref":)" + crate + R"(,"key":"tag_map","type":"probe.tag_map"})");
	BOOST_REQUIRE_MESSAGE(added.ok, added.error);

	const auto refused = call("component.set", R"({"ref":)" + crate + R"(,"key":"tag_map","patch":{"icons":{"a":"res://ok.png","b/c":"user://x.png"}}})");
	BOOST_TEST(refused.code == cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(refused.error.find("icons/b~1c") != std::string::npos, refused.error);
	BOOST_TEST(!child(root(), 0xa4)->components.at("tag_map").overrides.contains("icons"));

	const auto fine = call("component.set", R"({"ref":)" + crate + R"(,"key":"tag_map","patch":{"icons":{"a":"res://ok.png"}}})");
	BOOST_CHECK_MESSAGE(fine.ok, fine.error);
}

// An inline desc is read with its __parent and its own references: each follows #697, by the schema of its __type. A type
// without a schema cannot be checked and is refused.
BOOST_AUTO_TEST_CASE(an_inline_desc_is_checked_by_the_schema_of_its_type)
{
	const std::string crate = ref({ "00000000000000a4" });
	BOOST_REQUIRE(call("component.add", R"({"ref":)" + crate
		+ R"(,"key":"mesh","type":"mesh_node_desc","fields":{"vx_end":3,"ind_end":3}})").ok);
	const auto material = [&](const std::string& inline_desc) {
		return call("component.set", R"({"ref":)" + crate + R"(,"key":"mesh","patch":{"material":)" + inline_desc + "}}");
	};

	const auto parent = material(R"({"__type":"material_desc","__parent":"user://m.desc"})");
	BOOST_TEST(parent.code == cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(parent.error.find("material/__parent") != std::string::npos, parent.error);

	const auto texture = material(R"({"__type":"material_desc","samplers":["res://t.png","user://saves/t.png"]})");
	BOOST_TEST(texture.code == cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(texture.error.find("material/samplers/1") != std::string::npos, texture.error);

	// Backslashes are what res::tag turns into the separator: the tag is judged as it would be loaded.
	const auto backslashed = material(R"({"__type":"material_desc","samplers":["user:\\\\x.png"]})");
	BOOST_TEST(backslashed.code == cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(backslashed.error.find("material/samplers/0") != std::string::npos, backslashed.error);

	BOOST_TEST(material(R"({"__type":"no_such_desc"})").code == cmd::codes::invalid_args);
	BOOST_TEST(!child(root(), 0xa4)->components.at("mesh").overrides.contains("material"));

	const auto fine = material(R"({"__type":"material_desc","__parent":"res://base_material.desc","samplers":["res://t.png",null]})");
	BOOST_CHECK_MESSAGE(fine.ok, fine.error);
}

// #740: a partial patch of an inline material may leave out its __type: the type comes from the component as the patch
// leaves it, and the material's references are checked by that schema. Over a tag there is no type to take.
BOOST_AUTO_TEST_CASE(a_partial_patch_of_an_inline_desc_takes_its_type_from_the_component)
{
	const std::string crate = ref({ "00000000000000a4" });
	BOOST_REQUIRE(call("component.add", R"({"ref":)" + crate
		+ R"(,"key":"mesh","type":"mesh_node_desc","fields":{"vx_end":3,"ind_end":3}})").ok);
	const auto set = [&](const std::string& patch) {
		return call("component.set", R"({"ref":)" + crate + R"(,"key":"mesh","patch":)" + patch + "}");
	};
	const auto inline_material = set(R"({"material":{"__type":"material_desc","__parent":"res://base_material.desc"}})");
	BOOST_REQUIRE_MESSAGE(inline_material.ok, inline_material.error);

	const auto texture = set(R"({"material":{"samplers":["res://t.png"]}})");
	BOOST_CHECK_MESSAGE(texture.ok, texture.error);
	const auto& material = child(root(), 0xa4)->components.at("mesh").overrides.at("material").as_object();
	BOOST_TEST(material.at("__type").as_string() == "material_desc");
	BOOST_TEST(material.at("samplers").as_array().size() == 1u);

	const auto refused = set(R"({"material":{"samplers":["user://saves/t.png"]}})");
	BOOST_TEST(refused.code == cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(refused.error.find("material/samplers/0") != std::string::npos, refused.error);
	const auto& kept = child(root(), 0xa4)->components.at("mesh").overrides.at("material").as_object();
	BOOST_TEST(kept.at("samplers").as_array().at(0).as_string() == "res://t.png");

	BOOST_REQUIRE(set(R"({"material":"res://materials/red.mat.desc"})").ok);
	BOOST_TEST(set(R"({"material":{"samplers":["res://t.png"]}})").code == cmd::codes::invalid_args);
	BOOST_TEST(child(root(), 0xa4)->components.at("mesh").overrides.at("material").as_string() == "res://materials/red.mat.desc");
}

// #773: a {guid, path} reference in a tag field is held to #697 where it leads; it is no inline desc, and an object that
// is not a good reference is refused with the reason. Without an asset index the path is what leads.
BOOST_AUTO_TEST_CASE(a_reference_object_follows_the_rule_of_a_tag)
{
	const std::string crate = ref({ "00000000000000a4" });
	BOOST_REQUIRE(call("component.add", R"({"ref":)" + crate
		+ R"(,"key":"mesh","type":"mesh_node_desc","fields":{"vx_end":3,"ind_end":3}})").ok);
	const auto set = [&](const std::string& patch) {
		return call("component.set", R"({"ref":)" + crate + R"(,"key":"mesh","patch":)" + patch + "}");
	};
	const std::string guid = R"("guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f")";

	const auto fine = set(R"({"geometry":{)" + guid + R"(,"path":"res://g.geom.desc"},"material":{)" + guid + R"(,"path":"res://m.desc"}})");
	BOOST_REQUIRE_MESSAGE(fine.ok, fine.error);
	BOOST_TEST(child(root(), 0xa4)->components.at("mesh").overrides.at("material").as_object().at("path").as_string() == "res://m.desc");

	for (const std::string& bad : {
		std::string(R"({"material":{)") + guid + R"(,"path":"user://saves/m.desc"}})",
		std::string(R"({"geometry":{)") + guid + R"(,"path":"res://g.geom.desc","extra":1}})",
		std::string(R"({"geometry":{"guid":"00000000-0000-0000-0000-000000000000"}})"),
		std::string(R"({"material":{"samplers":["res://t.png"]}})") }) {
		BOOST_TEST_CONTEXT(bad) {
			const auto refused = set(bad);
			BOOST_TEST(refused.code == cmd::codes::invalid_args);
		}
	}
	BOOST_TEST(child(root(), 0xa4)->components.at("mesh").overrides.at("material").as_object().at("path").as_string() == "res://m.desc");
}

namespace {

// An asset index that knows where some IDs are and keys no tag by an ID, so no cache entry moves.
struct paths_index final : res::asset_index
{
	std::map<res::asset_id, res::tag> paths;
	std::optional<res::asset_id> id_of(const res::tag&) const override { return std::nullopt; }
	std::optional<res::tag> path_of(const res::asset_id& id) const override
	{
		const auto found = paths.find(id);
		return found == paths.end() ? std::nullopt : std::optional{ found->second };
	}
};

}

// #773: a reference the patch gives a reference field replaces the value whole. Merged into an inline desc it would keep
// the __type (the old inline desc would load), merged into another reference it would keep the old guid.
BOOST_AUTO_TEST_CASE(a_reference_in_a_patch_replaces_the_value_whole)
{
	const std::string crate = ref({ "00000000000000a4" });
	BOOST_REQUIRE(call("component.add", R"({"ref":)" + crate
		+ R"(,"key":"mesh","type":"mesh_node_desc","fields":{"vx_end":3,"ind_end":3}})").ok);
	const auto set = [&](const std::string& at, const std::string& patch) {
		return call("component.set", R"({"ref":)" + at + R"(,"key":"mesh","patch":)" + patch + "}");
	};
	const json::value reference = json::parse(R"({"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://m.desc"})");
	const std::string as_patch = R"({"material":)" + json::serialize(reference) + "}";

	BOOST_REQUIRE(set(crate, R"({"material":{"__type":"material_desc","__parent":"res://base_material.desc"}})").ok);
	const auto over_inline = set(crate, as_patch);
	BOOST_REQUIRE_MESSAGE(over_inline.ok, over_inline.error);
	BOOST_TEST(child(root(), 0xa4)->components.at("mesh").overrides.at("material") == reference);

	const auto new_path = set(crate, R"({"material":{"path":"res://n.desc"}})");
	BOOST_REQUIRE_MESSAGE(new_path.ok, new_path.error);
	BOOST_TEST(child(root(), 0xa4)->components.at("mesh").overrides.at("material") == json::parse(R"({"path":"res://n.desc"})"));

	// Inside a prefab instance the reference is a record of its own that sets the field whole, after the component's.
	const std::string eye = ref({ "00000000000000e1", "00000000000000c1" }, "w1");
	BOOST_REQUIRE(set(eye, R"({"material":{"__type":"material_desc","__parent":"res://base_material.desc"}})").ok);
	const auto inside = set(eye, as_patch);
	BOOST_REQUIRE_MESSAGE(inside.ok, inside.error);
	const auto records_of = [&] {
		std::vector<scn::override_record> out;
		for (const scn::override_record& r : child(root(1), 0xe1)->overrides) {
			if (r.component == "mesh") out.push_back(r);
		}
		return out;
	};
	const auto whole = [](const std::vector<scn::override_record>& records, const json::value& value) {
		return std::ranges::any_of(records, [&](const scn::override_record& r) { return r.path == "/material" && r.value == value; });
	};
	const auto merged_material = [](const std::vector<scn::override_record>& records) {
		return std::ranges::any_of(records, [](const scn::override_record& r) {
			return r.path.empty() && r.value.is_object() && r.value.as_object().contains("material"); });
	};
	BOOST_TEST(whole(records_of(), reference));
	BOOST_TEST(!merged_material(records_of()));

	BOOST_REQUIRE(set(eye, R"({"material":{"path":"res://n.desc"}})").ok);
	BOOST_TEST(whole(records_of(), json::parse(R"({"path":"res://n.desc"})")));
	BOOST_TEST(!merged_material(records_of()));
	// An edit of another field keeps the reference a record of its own.
	BOOST_REQUIRE(set(eye, R"({"visible":false})").ok);
	BOOST_TEST(whole(records_of(), json::parse(R"({"path":"res://n.desc"})")));
	BOOST_TEST(!merged_material(records_of()));

	// The Inspector writes an instance's component through write_component with the component's schema: the same.
	namespace oe = edt::override_edit;
	const auto instance = std::ranges::find_if(root(1).children, [](const prefab_node& node) { return node.node_id == 0xe1u; });
	BOOST_REQUIRE(instance != root(1).children.end());
	prefab_node& owner = *instance;
	const std::vector<std::uint64_t> target{ 0xc1 };
	const auto base_node = oe::find_base_node(*assembler, owner, target);
	BOOST_REQUIRE(base_node.has_value());
	const json::object base_json = oe::component_json(base_node->components.at("mesh"));
	json::object edited = oe::effective_component(owner, target, "mesh", base_json);
	edited["visible"] = true;
	oe::write_component(owner, target, "mesh", base_json, edited, descs.get(), descs->find_schema("mesh_node_desc"));
	BOOST_TEST(whole(records_of(), json::parse(R"({"path":"res://n.desc"})")));
	BOOST_TEST(!merged_material(records_of()));

	// The other way: an inline desc where a reference was replaces it whole, on a node and in the records of an instance.
	const json::value inline_material = json::parse(R"({"__type":"material_desc","__parent":"res://base_material.desc"})");
	BOOST_REQUIRE(set(crate, R"({"material":)" + json::serialize(inline_material) + "}").ok);
	BOOST_TEST(child(root(), 0xa4)->components.at("mesh").overrides.at("material") == inline_material);
	const json::object over_reference{ { "material", reference } };
	oe::write_component(owner, target, "mesh", over_reference, json::object{ { "material", inline_material } }, descs.get(),
		descs->find_schema("mesh_node_desc"));
	BOOST_TEST(whole(records_of(), inline_material));
	BOOST_TEST(!merged_material(records_of()));
}

// #773: inside a map of structures a reference replaces the value too, and the records of an instance set it whole.
BOOST_AUTO_TEST_CASE(a_reference_inside_a_map_of_structures_replaces_the_value_whole)
{
	BOOST_REQUIRE(descs->register_schema(desc::parse_type_schema(json::parse(R"({"name":"probe.icon_slot","kind":"struct",
		"fields":[{"name":"icon","kind":"res_tag","default":""}]})").as_object())));
	descs->register_component_desc<slot_map_component>("probe.slot_map");
	const std::string crate = ref({ "00000000000000a4" });
	const auto added = call("component.add", R"({"ref":)" + crate + R"(,"key":"slots","type":"probe.slot_map",)"
		+ R"("fields":{"slots":{"a":{"icon":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://a.png"}}}}})");
	BOOST_REQUIRE_MESSAGE(added.ok, added.error);

	const auto moved = call("component.set", R"({"ref":)" + crate + R"(,"key":"slots","patch":{"slots":{"a":{"icon":{"path":"res://b.png"}}}}})");
	BOOST_REQUIRE_MESSAGE(moved.ok, moved.error);
	const json::value overrides(child(root(), 0xa4)->components.at("slots").overrides);
	const json::value* icon = desc::json_pointer_get(overrides, "/slots/a/icon");
	BOOST_REQUIRE(icon);
	BOOST_TEST(*icon == json::parse(R"({"path":"res://b.png"})"));

	const desc::type_schema* schema = descs->find_schema("probe.slot_map");
	BOOST_REQUIRE(schema);
	const json::value base = json::parse(R"({"slots":{"a":{"icon":{"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://a.png"}}}})");
	const json::value edited = json::parse(R"({"slots":{"a":{"icon":{"path":"res://b.png"}},"b":{"icon":"res://c.png"}}})");
	const std::vector<std::string> whole = edt::override_edit::whole_values(*descs, *schema, base, edited);
	BOOST_TEST((whole == std::vector<std::string>{ "/slots/a/icon" }));
}

// #773: with an asset index, component.set checks a reference where its guid leads and its written path too (a game
// without an index loads that one). A guid no file holds, without a path, leads nowhere.
BOOST_AUTO_TEST_CASE(a_reference_is_checked_by_its_guid_and_its_written_path)
{
	const std::string crate = ref({ "00000000000000a4" });
	BOOST_REQUIRE(call("component.add", R"({"ref":)" + crate
		+ R"(,"key":"mesh","type":"mesh_node_desc","fields":{"vx_end":3,"ind_end":3}})").ok);
	const auto set = [&](const std::string& material) {
		return call("component.set", R"({"ref":)" + crate + R"(,"key":"mesh","patch":{"material":)" + material + "}}");
	};
	auto index = std::make_shared<paths_index>();
	index->paths.emplace(*res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f"), res::tag{ "res://materials/moved.mat.desc" });
	rs->set_asset_index(index);
	const auto unknown = set(R"({"guid":"0b6c1d2e-3f40-4a51-8b62-7c83d94ea5f6"})");
	const auto written = set(R"({"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"user://saves/m.desc"})");
	const auto moved = set(R"({"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f","path":"res://materials/old.mat.desc"})");
	const auto known = set(R"({"guid":"3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f"})");
	rs->set_asset_index(nullptr);

	BOOST_TEST(unknown.code == cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(unknown.error.find("0b6c1d2e") != std::string::npos, unknown.error);
	BOOST_TEST(written.code == cmd::codes::invalid_args);
	BOOST_CHECK_MESSAGE(written.error.find("material/path") != std::string::npos, written.error);
	BOOST_CHECK_MESSAGE(moved.ok, moved.error);
	BOOST_CHECK_MESSAGE(known.ok, known.error);
}

BOOST_AUTO_TEST_SUITE_END()
