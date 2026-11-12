#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/adapters/scn_model_importer_adapter.h>
#include <snk/scene/scn_glm_json_convert.h>
#include <snk/scene/scn_mesh_node_desc.h>
#include <snk/scene/scn_model.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <entt/entt.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// scn::model_importer_adapter: Assimp -> prefab_desc conversion, driven by tiny in-memory models.

namespace json = boost::json;

namespace {

constexpr const char* OBJ_TRIANGLE = R"(# one textured triangle
v -1 -1 0
v 1 -1 0
v 0 1 0
vt 0 0
vt 1 0
vt 0.5 1
vn 0 0 1
o Tri
f 1/1/1 2/2/1 3/3/1
)";

constexpr const char* OBJ_NO_UV = R"(v -1 -1 0
v 1 -1 0
v 0 1 0
o Tri
f 1 2 3
)";

// Two objects sharing a name under one node. Assimp keeps both meshes; the same-named group
// is merged by some loaders, so the test only requires that no mesh child is lost.
constexpr const char* OBJ_TWO_SAME_NAME = R"(v -1 -1 0
v 1 -1 0
v 0 1 0
v 2 2 0
v 3 2 0
v 2 3 0
vt 0 0
vt 1 0
vt 0 1
g Tri
usemtl a
f 1/1 2/2 3/3
g Tri
usemtl b
f 4/1 5/2 6/3
)";

// glTF 2.0 (ASCII, buffer embedded as a data URI, generated once): node "Parent" with
// translation (1,2,3), +90 degree yaw and uniform scale 2, whose child "Child" (translated (0,5,0))
// carries a single-triangle mesh with a red material; animation "Move" translates "Parent"
// from x=0 at t=0 to x=10 at t=2.
constexpr const char* GLTF_TRIANGLE = R"GLTF({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"Parent","translation":[1,2,3],"rotation":[0,0.7071068,0,0.7071068],"scale":[2,2,2],"children":[1,2]},{"name":"Child","mesh":0,"translation":[0,5,0]},{"name":"Tilted","rotation":[0.23911761839433449,0.3696438106143861,-0.09904576054128762,0.8923991008325228]}],"meshes":[{"name":"TriMesh","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],"materials":[{"name":"Red","pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}}],"animations":[{"name":"Move","samplers":[{"input":4,"output":5,"interpolation":"LINEAR"}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]}],"buffers":[{"byteLength":136,"uri":"data:application/octet-stream;base64,AACAvwAAgL8AAAAAAACAPwAAgL8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAD8AAIA/AAABAAIAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAIEEAAAAAAAAAAA=="}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24},{"buffer":0,"byteOffset":96,"byteLength":6},{"buffer":0,"byteOffset":104,"byteLength":8},{"buffer":0,"byteOffset":112,"byteLength":24}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"},{"bufferView":4,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[2]},{"bufferView":5,"componentType":5126,"count":2,"type":"VEC3"}]})GLTF";

// The buffer GLTF_TRIANGLE embeds, as the bytes of a .bin file next to the model.
constexpr unsigned char GLTF_TRIANGLE_BIN[] = {
	0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
	0x00, 0x00, 0x80, 0xbf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x80, 0x3f,
	0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x41,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// GLTF_TRIANGLE with its buffer read from `uri` instead of embedded.
std::string gltf_with_buffer_uri(std::string_view uri)
{
	std::string text = GLTF_TRIANGLE;
	const std::size_t begin = text.find("data:application/octet-stream;base64,");
	const std::size_t end = text.find('"', begin);
	text.replace(begin, end - begin, uri);
	return text;
}

// The child of a node (or of the file root) with the given name; children are an array of named nodes.
const json::value& child_named(const json::value& node, std::string_view name)
{
	for (const auto& child : node.at("children").as_array()) {
		if (child.at("name").as_string() == name) return child;
	}
	throw std::out_of_range("no child named " + std::string(name));
}

const json::value& child_named(const json::object& node, std::string_view name)
{
	for (const auto& child : node.at("children").as_array()) {
		if (child.at("name").as_string() == name) return child;
	}
	throw std::out_of_range("no child named " + std::string(name));
}

bool has_child(const json::object& node, std::string_view name)
{
	for (const auto& child : node.at("children").as_array()) {
		if (child.at("name").as_string() == name) return true;
	}
	return false;
}

struct importer_fixture : test_util::scene_env
{
	importer_fixture()
	{
		rs->registrate_adapter<scn::model_importer_adapter>(scn::model_importer_adapter::INFO, std::ref(*descs), std::ref(*rs));
	}

	std::shared_ptr<scn::prefab_desc> import(const char* name, const char* text)
	{
		auto tag = res::tag{ std::string("memory://models/") + name };
		rs->store(tag, test_util::to_bytes(text));
		return require_loaded<scn::prefab_desc>(tag).get_sync();
	}

	json::object serialized(const scn::prefab_desc& d)
	{
		json::object o;
		d.serialize(o);
		return o;
	}

	static const json::object* first_mesh_node(const json::object& node)
	{
		if (const auto* components = node.if_contains("components"); components && components->is_object()) {
			if (const auto* mesh = components->as_object().if_contains("mesh_node_desc")) {
				return &mesh->as_object();
			}
		}
		if (const auto* children = node.if_contains("children"); children && children->is_array()) {
			for (const auto& child : children->as_array()) {
				if (const auto* found = first_mesh_node(child.as_object())) {
					return found;
				}
			}
		}
		return nullptr;
	}

	// Imports, as memory://models/<folder>/tex.obj, a triangle whose .mtl gives `reference` as its diffuse map: the tag
	// its texture desc loads, if it got one. A folder per call: the texture descs are named after the model's folder.
	std::optional<std::string> imported_diffuse_data(const std::string& folder, const std::string& reference)
	{
		rs->store(res::tag{ "memory://models/" + folder + "/tex.mtl" }, test_util::to_bytes("newmtl skin\nKd 1 1 1\nmap_Kd " + reference + "\n"));
		const std::string obj = "mtllib tex.mtl\nv -1 -1 0\nv 1 -1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0.5 1\nvn 0 0 1\no Tri\nusemtl skin\nf 1/1/1 2/2/1 3/3/1\n";
		const auto prefab = import((folder + "/tex.obj").c_str(), obj.c_str());
		BOOST_REQUIRE(prefab);
		const auto out = serialized(*prefab);
		const json::object* mesh = first_mesh_node(out);
		BOOST_REQUIRE(mesh);
		const auto& material = mesh->at("material").as_object();
		BOOST_TEST_MESSAGE(json::serialize(material));
		BOOST_REQUIRE(material.at("name").as_string() == "skin");
		const auto* samplers = material.if_contains("samplers");
		if (!samplers) {
			return std::nullopt;
		}
		const auto it = ram->files.find(res::tag{ std::string_view{ samplers->at(0).as_string() } });
		BOOST_REQUIRE(it != ram->files.end());
		const auto texture = json::parse(std::string_view(reinterpret_cast<const char*>(it->second.data()), it->second.size())).as_object();
		return std::string(texture.at("data").as_string());
	}

};

}

namespace {

struct warning_log
{
	warning_log()
	{
		sink = engine::add_log_sink([this](std::string_view, engine::log_level level, std::string_view message) {
			if (level == engine::log_level::warn) warnings.emplace_back(message);
		});
	}

	// What the desc reference readers warn when they keep the current reference (the test env has no
	// res://base_material.desc, so the material's parent may warn on its own).
	bool kept_a_reference() const
	{
		for (const auto& warning : warnings) {
			if (warning.find("keeping the current") != std::string::npos || warning.find("inline desc") != std::string::npos) return true;
		}
		return false;
	}

	std::vector<std::string> warnings;
	engine::log_sink_handle sink; // last: removed first
};

// The mesh_node_desc component of the first mesh of an imported model, as the assembler gets it: the data without __type.
json::object imported_mesh_component(importer_fixture& env, const scn::prefab_desc& prefab)
{
	const json::object out = env.serialized(prefab);
	const json::object* mesh = importer_fixture::first_mesh_node(out);
	BOOST_REQUIRE(mesh);
	json::object component = *mesh;
	component.erase("__type");
	BOOST_REQUIRE(component.at("material").is_object());
	return component;
}

}

BOOST_FIXTURE_TEST_SUITE(ScnModelImporterTests, importer_fixture)

// #720: the assembler reads a component into a desc without a tag, and the importer writes the material inline. Read by
// the generic schema reader, such a material was refused: the smoke_level robot lost its material.
BOOST_AUTO_TEST_CASE(Gltf_AMeshOfTheImportedModel_SpawnsWithItsMaterial_WithoutAWarning)
{
	const auto prefab = import("tri.gltf", GLTF_TRIANGLE);
	BOOST_REQUIRE(prefab);
	const json::object component = imported_mesh_component(*this, *prefab);
	warning_log log;
	entt::registry reg;
	const auto e = reg.create();
	assembler->assemble_and_apply(reg, e, std::string(scn::mesh_node_desc::__type), {}, component, "mesh");
	pump();
	const auto* material = reg.try_get<scn::material_desc_component>(e);
	BOOST_REQUIRE(material);
	BOOST_TEST(material->mlt_desc.is_valid());
	BOOST_TEST(desc::is_override_tag(material->mlt_desc.get_tag().view()));
	BOOST_TEST(reg.all_of<scn::mesh_component>(e));
	BOOST_TEST(!log.kept_a_reference());
}

// #720 changes nothing in the files of an imported model: the prefab keeps the mesh components as their JSON (the material
// inline), and a mesh read from one writes what the hand-written serializer wrote: the tags, the material as its memory copy.
BOOST_AUTO_TEST_CASE(Gltf_TheImportedPrefab_ResavesToTheSameBytes)
{
	const auto prefab = import("tri.gltf", GLTF_TRIANGLE);
	BOOST_REQUIRE(prefab);
	const std::string first = json::serialize(serialized(*prefab));
	const auto resaved = std::dynamic_pointer_cast<scn::prefab_desc>(descs->create_instance(std::string(scn::prefab_desc::__type), tag_of("resaved.desc")));
	BOOST_REQUIRE(resaved);
	resaved->deserialize(*descs, json::parse(first).as_object());
	BOOST_TEST(json::serialize(serialized(*resaved)) == first);

	json::object component = imported_mesh_component(*this, *prefab);
	const auto mesh = std::dynamic_pointer_cast<scn::mesh_node_desc>(descs->create_instance(std::string(scn::mesh_node_desc::__type), tag_of("mesh.desc")));
	BOOST_REQUIRE(mesh);
	mesh->deserialize(*descs, component);
	json::object written;
	mesh->serialize(written);
	std::vector<std::string> keys;
	for (const auto& member : written) keys.emplace_back(member.key());
	BOOST_TEST((keys == std::vector<std::string>{ "geometry", "vx_begin", "vx_end", "ind_begin", "ind_end", "material" }));
	BOOST_TEST(written.at("geometry") == component.at("geometry"));
	BOOST_TEST(written.at("ind_end") == component.at("ind_end"));
	BOOST_REQUIRE(written.at("material").is_string());
	BOOST_TEST(desc::is_override_tag(written.at("material").as_string()));
}

// An override of a node of the model (a hidden mesh) goes through serialize(parent) and back: the mesh keeps the parent's
// material, not a second copy of it, and nothing warns.
BOOST_AUTO_TEST_CASE(Gltf_AHiddenMeshOfTheImportedModel_SharesTheMaterialOfItsParent)
{
	const auto prefab = import("tri.gltf", GLTF_TRIANGLE);
	BOOST_REQUIRE(prefab);
	json::object file = imported_mesh_component(*this, *prefab);
	file["__type"] = std::string(scn::mesh_node_desc::__type);
	const auto parent = require_loaded<desc::desc_base>(put("mesh.desc", json::serialize(file)));
	BOOST_REQUIRE(parent.is_ready());
	const auto parent_mesh = std::dynamic_pointer_cast<scn::mesh_node_desc>(parent.get_sync());
	BOOST_REQUIRE(parent_mesh);
	BOOST_REQUIRE(parent_mesh->material.is_valid());

	warning_log log;
	entt::registry reg;
	const auto e = reg.create();
	assembler->assemble_and_apply(reg, e, std::string(scn::mesh_node_desc::__type), parent, json::object{ { "visible", false } }, "mesh");
	pump();
	const auto* material = reg.try_get<scn::material_desc_component>(e);
	BOOST_REQUIRE(material);
	BOOST_TEST(material->mlt_desc.get_tag().view() == parent_mesh->material.get_tag().view());
	BOOST_TEST(!log.kept_a_reference());
}

BOOST_AUTO_TEST_CASE(Obj_TriangleBecomesPrefabWithMeshNodeAndDefaultMaterial)
{
	auto prefab = import("tri.obj", OBJ_TRIANGLE);
	BOOST_REQUIRE(prefab);
	auto out = serialized(*prefab);
	const auto& mesh = child_named(child_named(out, "Tri"), "mesh_Tri").at("components").at("mesh_node_desc").as_object();
	BOOST_TEST(mesh.at("vx_begin").to_number<int>() == 0);
	BOOST_TEST(mesh.at("vx_end").to_number<int>() == 3);
	BOOST_TEST(mesh.at("ind_begin").to_number<int>() == 0);
	BOOST_TEST(mesh.at("ind_end").to_number<int>() == 3);
	BOOST_TEST(mesh.at("geometry").as_string() == "memory://models/tri.geom.desc");
	const auto& mat = mesh.at("material").as_object();
	BOOST_TEST(mat.at("__type").as_string() == "material_desc");
	BOOST_TEST(mat.at("name").as_string() == "DefaultMaterial");
	BOOST_TEST(mat.at("queue").as_string() == "mix");
	BOOST_TEST(mat.at("defines").at(0).as_string() == "LIGHTS_ENABLED");
}

BOOST_AUTO_TEST_CASE(Obj_StoresGeometryDescWithLayoutIndicesAndVertexBytes)
{
	import("tri.obj", OBJ_TRIANGLE);
	auto it = ram->files.find(res::tag{ "memory://models/tri.geom.desc" });
	BOOST_REQUIRE(it != ram->files.end());
	auto geom = json::parse(std::string_view(reinterpret_cast<const char*>(it->second.data()), it->second.size())).as_object();
	BOOST_TEST(geom.at("__type").as_string() == "geometry_desc");
	BOOST_TEST(geom.at("layout").at("position").as_string() == "vec3");
	BOOST_TEST(geom.at("layout").at("uv").as_string() == "vec2");
	BOOST_TEST(geom.at("indices").as_array().size() == 3u);
	// stride = pos3 + normal3 + uv2 + tangent3 + bitangent3 floats
	BOOST_TEST(geom.at("vertices").as_array().size() == 3u * 14u * sizeof(float));
}

BOOST_AUTO_TEST_CASE(Obj_RegistersAliasFromSourceTagToStoredPrefab)
{
	import("tri.obj", OBJ_TRIANGLE);
	auto aliased = rs->resolve_alias(res::tag{ "memory://models/tri.obj" });
	BOOST_TEST(aliased != res::tag{ "memory://models/tri.obj" });
	BOOST_TEST(aliased.extension() == "desc");
	BOOST_TEST(rs->exists(aliased));
}

BOOST_AUTO_TEST_CASE(Obj_StoredPrefabTagHasNoDoubleSlash)
{
	import("tri.obj", OBJ_TRIANGLE);
	auto aliased = rs->resolve_alias(res::tag{ "memory://models/tri.obj" });
	BOOST_TEST(aliased.string().find("//", 9) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(Gltf_NodeTransformsAndHierarchyAreImported)
{
	auto prefab = import("tri.gltf", GLTF_TRIANGLE);
	BOOST_REQUIRE(prefab);
	auto out = serialized(*prefab);
	BOOST_TEST((json::value_to<glm::vec3>(out.at("transform").at("position")) == glm::vec3{ 1, 2, 3 }));
	auto scale = json::value_to<glm::vec3>(out.at("transform").at("scale"));
	BOOST_TEST(scale.y == 2.0f, boost::test_tools::tolerance(1e-4f));
	BOOST_TEST(has_child(out, "Child"));
	BOOST_TEST(has_child(out, "Tilted"));
	const auto& child = child_named(out, "Child");
	BOOST_TEST((json::value_to<glm::vec3>(child.at("transform").at("position")) == glm::vec3{ 0, 5, 0 }));
	BOOST_TEST(child_named(child, "mesh_TriMesh").at("components").at("mesh_node_desc").at("material").at("name").as_string() == "Red");
}

BOOST_AUTO_TEST_CASE(Gltf_MaterialColorBecomesAlbedoUniform)
{
	auto prefab = import("tri.gltf", GLTF_TRIANGLE);
	auto out = serialized(*prefab);
	const auto& mat = child_named(child_named(out, "Child"), "mesh_TriMesh").at("components").at("mesh_node_desc").at("material");
	BOOST_TEST((json::value_to<glm::vec4>(mat.at("uniforms").at("albedo")) == glm::vec4{ 1, 0, 0, 1 }));
}

BOOST_AUTO_TEST_CASE(Gltf_AnimationBecomesInlineClipAndMarksAnimatedNodes)
{
	auto prefab = import("tri.gltf", GLTF_TRIANGLE);
	auto out = serialized(*prefab);
	const auto& comps = out.at("components").as_object();
	BOOST_TEST(comps.contains("animated_node_desc"));   // root "Parent" is the animated node
	const auto& clip = comps.at("animation_collection_desc").at("clips").at(0).as_object();
	BOOST_TEST(clip.at("name").as_string() == "Move");
	BOOST_TEST(clip.at("channels").as_object().contains("Parent"));
	const auto& pos = clip.at("channels").at("Parent").at("position").as_array();
	BOOST_REQUIRE(pos.size() == 2u);
	BOOST_TEST((json::value_to<glm::vec3>(pos[1].at("value")) == glm::vec3{ 10, 0, 0 }));
	BOOST_TEST(pos[1].at("time").to_number<double>() == clip.at("duration").to_number<double>());
}

BOOST_AUTO_TEST_CASE(Gltf_CombinedRotationRoundTripsThroughPrefabTransform)
{
	auto prefab = import("tri.gltf", GLTF_TRIANGLE);
	const scn::prefab_desc::prefab_node* tilted = nullptr;
	for (auto& c : prefab->get_root().children) if (c.name == "Tilted") tilted = &c;
	BOOST_REQUIRE(tilted);
	glm::quat q(0.8923991f, 0.2391176f, 0.3696438f, -0.0990458f);
	glm::mat3 expected = glm::mat3_cast(q);
	glm::mat3 actual(tilted->get_local_transform());
	for (int c = 0; c < 3; ++c)
		for (int r = 0; r < 3; ++r)
			BOOST_TEST(std::abs(actual[c][r] - expected[c][r]) < 1e-3f);
}

BOOST_AUTO_TEST_CASE(Gltf_NinetyDegreeYawRoundTripsThroughPrefabTransform)
{
	auto prefab = import("tri.gltf", GLTF_TRIANGLE);
	glm::mat3 expected = glm::mat3_cast(glm::quat(0.7071068f, 0.0f, 0.7071068f, 0.0f)); // w x y z
	glm::mat3 m(prefab->get_root().get_local_transform());
	glm::mat3 rot(m[0] / 2.0f, m[1] / 2.0f, m[2] / 2.0f); // remove uniform scale 2
	float diff = 0;
	for (int c = 0; c < 3; ++c) diff += glm::length(rot[c] - expected[c]);
	BOOST_TEST(diff < 1e-2f);
}

BOOST_AUTO_TEST_CASE(Obj_MeshWithoutUvsImportsWithZeroUvBounds)
{
	auto prefab = import("nouv.obj", OBJ_NO_UV);
	BOOST_REQUIRE(prefab);
	auto out = serialized(*prefab);
	BOOST_TEST(child_named(child_named(out, "Tri"), "mesh_Tri").at("components").at("mesh_node_desc").at("ind_end").to_number<int>() == 3);
}

BOOST_AUTO_TEST_CASE(Obj_SameNamedMeshesUnderOneNodeGetUniqueChildren)
{
	auto prefab = import("twin.obj", OBJ_TWO_SAME_NAME);
	BOOST_REQUIRE(prefab);
	auto out = serialized(*prefab);
	std::size_t mesh_children = 0;
	for (const auto& value : out.at("children").as_array())
		for (const auto& child : value.at("children").as_array())
			if (child.at("name").as_string().starts_with("mesh_")) ++mesh_children;
	BOOST_TEST_MESSAGE(json::serialize(out));
	BOOST_TEST(mesh_children == 2u);
}

BOOST_AUTO_TEST_CASE(Garbage_DataYieldsNoPrefab)
{
	BOOST_TEST(!import("broken.gltf", "this is not a model"));
}

// #631: a model names its files relative to itself; they are read in its folder and protocol, never above the root.
BOOST_AUTO_TEST_CASE(Obj_ATextureWrittenRelativeToTheModel_IsReadInTheModelsFolder)
{
	int index = 0;
	for (const auto& [reference, expected] : std::initializer_list<std::pair<const char*, const char*>>{
		{ "textures/a.png", "memory://models/c0/textures/a.png" },
		{ "textures\\a.png", "memory://models/c1/textures/a.png" },
		{ "./textures/../a.png", "memory://models/c2/a.png" },
		{ "../shared/a.png", "memory://models/shared/a.png" } }) {
		BOOST_TEST_CONTEXT(reference) {
			const std::optional<std::string> data = imported_diffuse_data(std::format("c{}", index++), reference);
			BOOST_REQUIRE(data.has_value());
			BOOST_TEST(*data == expected);
		}
	}
}

// #631: a .bin the model names is opened through the importer's IO system, in the model's folder and never above the root.
BOOST_AUTO_TEST_CASE(Gltf_ABufferNextToTheModel_IsRead_OneAboveTheRootIsNot)
{
	const std::string bin(reinterpret_cast<const char*>(GLTF_TRIANGLE_BIN), sizeof(GLTF_TRIANGLE_BIN));
	rs->store(res::tag{ "memory://models/g0/data/tri.bin" }, test_util::to_bytes(bin));
	const auto beside = import("g0/tri.gltf", gltf_with_buffer_uri("data/tri.bin").c_str());
	BOOST_REQUIRE(beside);
	BOOST_TEST(has_child(serialized(*beside), "Child"));

	rs->store(res::tag{ "memory://tri.bin" }, test_util::to_bytes(bin));
	BOOST_TEST(!import("g1/tri.gltf", gltf_with_buffer_uri("../../../tri.bin").c_str()));
	BOOST_TEST(!import("g2/tri.gltf", gltf_with_buffer_uri("../c:/tri.bin").c_str()));
}

BOOST_AUTO_TEST_CASE(Obj_ATextureAboveTheProtocolsRootOrAbsolute_IsNotLoaded)
{
	int index = 0;
	for (const char* reference : { "../../../a.png", "../../../../etc/passwd.png", "/etc/a.png", "C:/a.png", "C:\\a.png",
		"../c:/a.png", "a.png:stream", "\\\\server\\share\\a.png", "..\\..\\..\\a.png" }) {
		BOOST_TEST_CONTEXT(reference) {
			BOOST_TEST(!imported_diffuse_data(std::format("r{}", index++), reference).has_value());
		}
	}
}

BOOST_AUTO_TEST_SUITE_END()
