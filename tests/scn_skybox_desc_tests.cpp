#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <boost/json.hpp>
#include <memory>
#include <sstream>
#include <string>

// skybox_desc through the generic schema (#131-d): the bytes of the shipped file, every form of the material key, the
// material as a tag and as an inline object (also across a save into a fresh system), and the declaration of the schema.

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

std::shared_ptr<scn::skybox_desc> make_skybox(test_util::scene_env& env, std::string_view name = "sky.desc")
{
	return std::dynamic_pointer_cast<scn::skybox_desc>(env.descs->create_instance("skybox_desc", test_util::scene_env::tag_of(name)));
}

// core/res/skybox/sky_material.desc, header keys as they are on disk.
constexpr const char* k_sky_material = R"({"__type":"material_desc","__version":1,"__guid":"ef0eb176-c36e-4b82-b6df-4e2c8dca625c",
	"defines":[],"samplers":["res://skybox/sky_texture_cubemap.desc"],"shader_vertex":"res://shaders/sky.vert",
	"shader_fragment":"res://shaders/sky.frag","shader_geometry":null,"queue":"opaque",
	"uniforms":{"albedo":[1.0,1.0,1.0,1.0],"emissive":[0.0,0.0,0.0,0.0]},"render_mode":"TRIANGLE"})";

constexpr const char* k_inline_material = R"({"__type":"material_desc","queue":"transparent","shader_vertex":"res://shaders/sky.vert",
	"uniforms":{"albedo":[0.5,0.25,0.5,1.0]}})";

std::string with_material(const std::string& value)
{
	return "{\"material\":" + value + "}";
}

}

BOOST_FIXTURE_TEST_SUITE(ScnSkyboxDescTests, test_util::scene_env)

// core/res/skybox/skybox.desc: the generic writer must give the bytes the hand-written one gave (desc_resave --check holds
// the file itself, desc_repo_roundtrip_tests the load).
BOOST_AUTO_TEST_CASE(SkyboxDesc_ShippedFileResavesToTheSameBytes)
{
	// the res:// files of the repository from RAM, as desc_repo_roundtrip_tests does
	rs->unregistrate_resolver(res::tag::default_protocol());
	auto& shipped = rs->registrate_resolver<test_util::ram_resolver>(res::tag::default_protocol());
	shipped.files[res::tag::make("skybox/sky_material.desc")] = test_util::to_bytes(k_sky_material);

	auto sky = make_skybox(*this, "skybox.desc");
	BOOST_REQUIRE(sky);
	sky->deserialize(*descs, parse(R"({"__type":"skybox_desc","__version":1,"__guid":"6c2ea994-a502-4240-8e8e-8c0cea6d73a9",
		"material":"res://skybox/sky_material.desc"})"));

	BOOST_TEST(json::serialize(ser(*sky)) == R"({"material":"res://skybox/sky_material.desc"})");

	std::ostringstream text;
	descs->write_pretty_json(text, ser(*sky));
	BOOST_TEST(text.str() == "{\n"
		"    \"material\" : \"res://skybox/sky_material.desc\"\n"
		"}\n");

	// and the reference leads to the shipped material
	pump();
	const auto material = sky->material.get_sync();
	BOOST_REQUIRE(material);
	BOOST_TEST(material->vertex_shader.string() == "res://shaders/sky.vert");
	BOOST_TEST((material->queue == scn::pass_queue::OPAQUE));
}

BOOST_AUTO_TEST_CASE(SkyboxDesc_ReadsTheMaterialAsATag)
{
	const res::tag material = put("sky_material.desc", k_sky_material);
	auto sky = make_skybox(*this);
	sky->deserialize(*descs, parse(with_material("\"" + material.string() + "\"")));

	BOOST_REQUIRE(sky->material.is_valid());
	BOOST_TEST((sky->material.get_tag() == material));
	pump();
	const auto loaded = sky->material.get_sync();
	BOOST_REQUIRE(loaded);
	BOOST_TEST(loaded->fragment_shader.string() == "res://shaders/sky.frag");
	// a tag is written as a tag
	BOOST_TEST(std::string_view{ ser(*sky).at("material").as_string() } == material.view());
}

BOOST_AUTO_TEST_CASE(SkyboxDesc_WithoutAMaterialWritesNoKey)
{
	auto sky = make_skybox(*this);
	sky->deserialize(*descs, parse("{}"));
	BOOST_TEST(!sky->material.is_valid());
	BOOST_TEST(ser(*sky).empty());

	// the header keys and the keys nobody knows are not fields
	sky->deserialize(*descs, parse(R"({"__type":"skybox_desc","__version":1,"__guid":"x","exposure":2})"));
	BOOST_TEST(!sky->material.is_valid());
	BOOST_TEST(ser(*sky).empty());
}

BOOST_AUTO_TEST_CASE(SkyboxDesc_AnEmptyTagClearsTheMaterial)
{
	const res::tag material = put("sky_material.desc", k_sky_material);
	auto sky = make_skybox(*this);
	sky->deserialize(*descs, parse(with_material("\"" + material.string() + "\"")));
	BOOST_REQUIRE(sky->material.is_valid());

	sky->deserialize(*descs, parse(with_material("\"\"")));
	BOOST_TEST(!sky->material.is_valid());
	BOOST_TEST(ser(*sky).empty());
}

// The hand-written reader asserted on these; the schema warns and keeps what the skybox has.
BOOST_AUTO_TEST_CASE(SkyboxDesc_ADamagedMaterialKeepsTheCurrentOne)
{
	const res::tag material = put("sky_material.desc", k_sky_material);
	auto sky = make_skybox(*this);
	sky->deserialize(*descs, parse(with_material("\"" + material.string() + "\"")));

	for (const char* damaged : { "5", "null", "[\"a\"]", "{\"queue\":\"mix\"}", "true" }) {
		BOOST_TEST_CONTEXT(damaged) {
			sky->deserialize(*descs, parse(with_material(damaged)));
			BOOST_REQUIRE(sky->material.is_valid());
			BOOST_TEST((sky->material.get_tag() == material));
		}
	}
}

BOOST_AUTO_TEST_CASE(SkyboxDesc_ReadsTheMaterialAsAnInlineObjectAndWritesItInline)
{
	auto sky = make_skybox(*this);
	const json::object data = parse(with_material(k_inline_material));
	sky->deserialize(*descs, data);

	const res::tag expected = descs->make_mem_tag_deterministic(*sky, "material_desc", data.at("material"));
	BOOST_REQUIRE(sky->material.is_valid());
	BOOST_TEST((sky->material.get_tag() == expected));
	BOOST_TEST(ram->files.contains(expected));
	pump();
	const auto loaded = sky->material.get_sync();
	BOOST_REQUIRE(loaded);
	BOOST_TEST((loaded->queue == scn::pass_queue::TRANSPARENT));
	BOOST_CHECK((loaded->albedo == ds::color{ 0.5f, 0.25f, 0.5f, 1.0f }));

	// the memory copy lives only in this process, so the skybox is saved with the object
	const json::object saved = ser(*sky);
	BOOST_REQUIRE(saved.at("material").is_object());
	const auto& written = saved.at("material").as_object();
	BOOST_TEST(written.at("__type").as_string() == "material_desc");
	BOOST_TEST(written.at("queue").as_string() == "transparent");
	BOOST_TEST(written.at("shader_vertex").as_string() == "res://shaders/sky.vert");
}

BOOST_AUTO_TEST_CASE(SkyboxDesc_AnInlineMaterialNeedsASkyboxWithATag)
{
	auto untagged = std::dynamic_pointer_cast<scn::skybox_desc>(descs->create_instance("skybox_desc", res::tag{}));
	BOOST_REQUIRE(untagged);
	untagged->deserialize(*descs, parse(with_material(k_inline_material)));
	BOOST_TEST(!untagged->material.is_valid());
}

BOOST_AUTO_TEST_CASE(SkyboxDesc_CopyToCarriesTheMaterial)
{
	const res::tag material = put("sky_material.desc", k_sky_material);
	auto from = make_skybox(*this, "from.desc");
	from->deserialize(*descs, parse(with_material("\"" + material.string() + "\"")));

	auto to = make_skybox(*this, "to.desc");
	from->copy_to(*to);
	BOOST_REQUIRE(to->material.is_valid());
	BOOST_TEST((to->material.get_tag() == material));
	BOOST_TEST(json::value(ser(*to)) == json::value(ser(*from)));
}

BOOST_AUTO_TEST_CASE(SkyboxDesc_SchemaDeclaresTheKeyAndThePolicy)
{
	const desc::type_schema* schema = descs->find_schema(scn::skybox_desc::__type);
	BOOST_REQUIRE(schema);
	BOOST_CHECK(schema->is_component);
	BOOST_REQUIRE_EQUAL(schema->fields.size(), 1u);

	const desc::field_schema& material = schema->fields[0];
	BOOST_TEST(material.name == "material");
	BOOST_TEST(material.key == "/material");
	BOOST_CHECK(material.kind == desc::field_kind::desc_ref);
	BOOST_TEST(material.ref_type == "material_desc");
	BOOST_CHECK(material.write == desc::write_policy::if_valid);
	BOOST_CHECK(material.default_value.is_null());

	// the type the field points to is a schema of its own
	BOOST_CHECK(descs->find_schema(scn::material_desc::__type) != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()

// Two systems one after the other, so no fixture.
BOOST_AUTO_TEST_SUITE(ScnSkyboxDescFreshSystemTests)

BOOST_AUTO_TEST_CASE(SkyboxDesc_InlineMaterialSurvivesASaveIntoAFreshSystem)
{
	json::object saved;
	{
		test_util::scene_env first;
		auto sky = make_skybox(first);
		BOOST_REQUIRE(sky);
		sky->deserialize(*first.descs, parse(with_material(k_inline_material)));
		saved = ser(*sky);
	}
	BOOST_REQUIRE(saved.at("material").is_object());

	test_util::scene_env second;
	auto reloaded = make_skybox(second);
	BOOST_REQUIRE(reloaded);
	reloaded->deserialize(*second.descs, saved);
	BOOST_REQUIRE(reloaded->material.is_valid());
	second.pump();
	const auto material = reloaded->material.get_sync();
	BOOST_REQUIRE(material);
	BOOST_TEST((material->queue == scn::pass_queue::TRANSPARENT));
	BOOST_CHECK((material->albedo == ds::color{ 0.5f, 0.25f, 0.5f, 1.0f }));
	BOOST_TEST(json::value(ser(*reloaded)) == json::value(saved));
}

BOOST_AUTO_TEST_SUITE_END()
