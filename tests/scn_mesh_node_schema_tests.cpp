#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/scn_mesh_node_desc.h>
#include <boost/json.hpp>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

// mesh_node_desc through the generic schema (#720): the keys, their order and when each is written are those the
// hand-written serializer gave, so a resaved mesh of an imported model or of a shipped level keeps its bytes.

namespace json = boost::json;

namespace {

json::object parse(const std::string& text)
{
	return json::parse(text).as_object();
}

json::object ser(const desc::desc_base& d)
{
	json::object o;
	d.serialize(o);
	return o;
}

std::shared_ptr<scn::mesh_node_desc> make_mesh(test_util::scene_env& env, std::string_view name = "mesh.desc")
{
	return std::dynamic_pointer_cast<scn::mesh_node_desc>(env.descs->create_instance("mesh_node_desc", test_util::scene_env::tag_of(name)));
}

std::string resaved(test_util::scene_env& env, const std::string& text)
{
	auto mesh = make_mesh(env);
	BOOST_REQUIRE(mesh);
	mesh->deserialize(*env.descs, parse(text));
	return json::serialize(ser(*mesh));
}

}

BOOST_FIXTURE_TEST_SUITE(ScnMeshNodeSchemaTests, test_util::scene_env)

// A mesh node of projects/sandbox/res/levels/bench_100_meshes.desc (and of an imported model, which writes the same keys).
BOOST_AUTO_TEST_CASE(AMeshOfTheBenchLevel_ResavesToTheSameBytes)
{
	const std::string written = resaved(*this, R"({"__type":"mesh_node_desc","__version":1,"geometry":"res://bench/bench_quad.desc",
		"vx_begin":0,"vx_end":4,"ind_begin":0,"ind_end":6,"material":"res://base_material.desc"})");
	BOOST_TEST(written == R"({"geometry":"res://bench/bench_quad.desc","vx_begin":0,"vx_end":4,"ind_begin":0,"ind_end":6,)"
		R"("material":"res://base_material.desc"})");

	auto mesh = make_mesh(*this);
	mesh->deserialize(*descs, parse(written));
	std::ostringstream text;
	descs->write_pretty_json(text, ser(*mesh));
	BOOST_TEST(text.str() == "{\n"
		"    \"geometry\" : \"res://bench/bench_quad.desc\",\n"
		"    \"vx_begin\" : 0,\n"
		"    \"vx_end\" : 4,\n"
		"    \"ind_begin\" : 0,\n"
		"    \"ind_end\" : 6,\n"
		"    \"material\" : \"res://base_material.desc\"\n"
		"}\n");
}

// visible is written only when false, last; geometry and material only when set; the ranges always.
BOOST_AUTO_TEST_CASE(OnlyAHiddenMeshWritesVisible_AndAnEmptyReferenceIsLeftOut)
{
	BOOST_TEST(resaved(*this, R"({"vx_end":3,"ind_end":3,"visible":false})")
		== R"({"vx_begin":0,"vx_end":3,"ind_begin":0,"ind_end":3,"visible":false})");
	BOOST_TEST(resaved(*this, R"({"vx_end":3,"ind_end":3,"visible":true})")
		== R"({"vx_begin":0,"vx_end":3,"ind_begin":0,"ind_end":3})");
	BOOST_TEST(resaved(*this, R"({"geometry":"","material":""})")
		== R"({"vx_begin":0,"vx_end":0,"ind_begin":0,"ind_end":0})");
}

// What the hand-written reader took: a value of the wrong type keeps the field's default instead of failing the desc.
BOOST_AUTO_TEST_CASE(AFieldOfTheWrongType_KeepsItsDefault)
{
	auto mesh = make_mesh(*this);
	mesh->deserialize(*descs, parse(R"({"vx_end":4,"visible":"no"})"));
	BOOST_TEST(mesh->visible);
	BOOST_TEST(mesh->vx_end == 4u);
}

// The assembler reads a component into a desc without a tag; the importer writes the material inline (the robot of
// smoke_level). The generic reader refuses an inline desc for an untagged owner, the mesh keeps the material.
BOOST_AUTO_TEST_CASE(AnUntaggedMesh_KeepsTheInlineMaterialOfAnImportedModel)
{
	auto mesh = std::dynamic_pointer_cast<scn::mesh_node_desc>(descs->create_instance("mesh_node_desc", res::tag{}));
	BOOST_REQUIRE(mesh);
	mesh->deserialize(*descs, parse(R"({"vx_end":3,"ind_end":3,
		"material":{"__type":"material_desc","__parent":"res://base_material.desc","queue":"mix"}})"));
	BOOST_TEST(mesh->material.is_valid());
	BOOST_TEST(desc::is_override_tag(mesh->material.get_tag().view()));
	BOOST_TEST(mesh->ind_end == 3u);
}

// The schema is what component.set and the pre-Play check read: its two references are desc references to the geometry
// and the material.
BOOST_AUTO_TEST_CASE(TheSchema_NamesTheTwoReferencesAndTheSevenFields)
{
	const desc::type_schema* schema = descs->find_schema("mesh_node_desc");
	BOOST_REQUIRE(schema);
	BOOST_TEST(schema->fields.size() == 7u);
	const auto field = [schema](std::string_view key) {
		const desc::field_schema* found = schema->find_field(key);
		return found ? found : schema->find_field("/" + std::string(key));
	};
	BOOST_REQUIRE(field("geometry"));
	BOOST_TEST(field("geometry")->ref_type == "geometry_desc");
	BOOST_REQUIRE(field("material"));
	BOOST_TEST(field("material")->ref_type == "material_desc");
}

BOOST_AUTO_TEST_SUITE_END()
