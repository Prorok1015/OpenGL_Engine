#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <boost/json.hpp>
#include <snk/desc/desc_describe.h>
#include <snk/desc/desc_schema.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include <snk/scene/scn_material_desc.h>
#include <snk/render/rnd_state_helper.h>
#include <snk/model_codec/res_tag_json.h>
#include <snk/model_codec/scn_glm_json_convert.h>

namespace json = boost::json;

BOOST_AUTO_TEST_SUITE(ScnMaterialDescTests)

BOOST_AUTO_TEST_CASE(MaterialDesc_TypeConstant)
{
	BOOST_TEST(scn::material_desc::__type == "material_desc");
}

// material_desc::deserialize only hands its desc_system argument on to the schema reader as the context of desc_ref
// fields, and its one desc_ref (shader, #1200) is only read when the key is there: these cases never write it. Constructing
// a real desc_system here is impossible — the project's tests deliberately avoid resource_system, which asserts on a real
// resources path. So we pass a reference that is never dereferenced; the shader reference is tested in
// rnd_shader_desc_tests.cpp and scn_desc_roundtrip_tests.cpp.
static desc::desc_system& shared_desc_system()
{
	// Aligned, non-null storage (never constructed or dereferenced): binding a reference to null trips UBSan.
	// (desc_system is an incomplete type here, so use max-aligned dummy bytes.)
	alignas(std::max_align_t) static unsigned char dummy_bytes[64];
	return *reinterpret_cast<desc::desc_system*>(dummy_bytes);
}

BOOST_AUTO_TEST_CASE(MaterialDesc_RoundTrip_Scalars)
{
	scn::material_desc src;
	src.defines = { "USE_NORMAL_MAP", "USE_SPECULAR_MAP" };
	src.queue = scn::pass_queue::MIX;
	src.albedo = ds::color{ 0.1f, 0.2f, 0.3f, 0.4f };
	src.emissive = ds::color{ 0.5f, 0.6f, 0.7f, 0.8f };
	src.shininess = 48.0f;
	src.samplers_textures = { res::tag{ "res://tex0.desc" }, res::tag{ "res://tex1.desc" } };

	json::object data;
	src.serialize(data);

	scn::material_desc dst;
	dst.deserialize(shared_desc_system(), data);

	BOOST_TEST(!data.contains("shader"));
	BOOST_TEST(!dst.shader.is_valid());
	BOOST_TEST(dst.defines == src.defines);
	BOOST_TEST((dst.queue == scn::pass_queue::MIX));
	BOOST_TEST(dst.albedo == src.albedo);
	BOOST_TEST(dst.emissive == src.emissive);
	BOOST_TEST(dst.shininess == 48.0f); // #321: a non-default shininess is written and read back
	BOOST_TEST(dst.samplers_textures.size() == src.samplers_textures.size());
	BOOST_TEST(dst.samplers_textures[0] == src.samplers_textures[0]);
	BOOST_TEST(dst.samplers_textures[1] == src.samplers_textures[1]);
}

BOOST_AUTO_TEST_CASE(MaterialDesc_RoundTrip_AllRenderModes)
{
	using rnd::driver::RENDER_MODE;
	const RENDER_MODE modes[] = {
		RENDER_MODE::TRIANGLE,
		RENDER_MODE::TRIANGLE_STRIP,
		RENDER_MODE::LINE,
		RENDER_MODE::LINE_STRIP,
		RENDER_MODE::POINT,
	};

	for (auto mode : modes) {
		scn::material_desc src;
		src.render_mode = mode;

		json::object data;
		src.serialize(data);

		scn::material_desc dst;
		dst.deserialize(shared_desc_system(), data);

		BOOST_TEST((dst.render_mode == mode));
	}
}

BOOST_AUTO_TEST_CASE(MaterialDesc_RenderMode_StringTable)
{
	// The strings are the names of the driver's enumerators, the same ones rnd::to_string gives.
	for (const auto mode : { rnd::driver::RENDER_MODE::TRIANGLE, rnd::driver::RENDER_MODE::TRIANGLE_STRIP, rnd::driver::RENDER_MODE::LINE,
			rnd::driver::RENDER_MODE::LINE_STRIP, rnd::driver::RENDER_MODE::POINT }) {
		scn::material_desc m;
		m.render_mode = mode;
		json::object data;
		m.serialize(data);
		BOOST_TEST(json::value_to<std::string>(data.at("render_mode")) == rnd::to_string(mode));
	}
}

BOOST_AUTO_TEST_CASE(MaterialDesc_RemovedRenderModesReadAsTriangle)
{
	// RHI R4 removed these topologies from the driver; a file that still names one is drawn as triangles.
	for (const char* name : { "TRIANGLE_FAN", "TRIANGLE_ADJ", "TRIANGLE_STRIP_ADJ", "LINE_LOOP", "LINE_ADJ", "LINE_STRIP_ADJ" }) {
		json::object data;
		data["render_mode"] = name;

		scn::material_desc material;
		material.render_mode = rnd::driver::RENDER_MODE::POINT;
		material.deserialize(shared_desc_system(), data);

		BOOST_TEST_CONTEXT(name) BOOST_TEST((material.render_mode == rnd::driver::RENDER_MODE::TRIANGLE));
	}
}

BOOST_AUTO_TEST_CASE(MaterialDesc_QueueValues_RoundTrip)
{
	const scn::pass_queue queues[] = { scn::pass_queue::OPAQUE, scn::pass_queue::TRANSPARENT, scn::pass_queue::MIX };
	for (auto q : queues) {
		scn::material_desc src;
		src.queue = q;
		json::object data;
		src.serialize(data);
		scn::material_desc dst;
		dst.deserialize(shared_desc_system(), data);
		BOOST_TEST((dst.queue == q));
	}
}

BOOST_AUTO_TEST_SUITE_END()

// #1196: the material is a data document (the editor's material window); its positional `samplers` is described by its codec.
BOOST_FIXTURE_TEST_SUITE(ScnMaterialDescDocumentTests, test_util::scene_env)

BOOST_AUTO_TEST_CASE(MaterialDesc_IsADataDocumentType_ThroughTheCodecOfItsSamplers)
{
	const desc::type_schema* schema = descs->find_schema("material_desc");
	BOOST_REQUIRE(schema);
	BOOST_TEST(schema->is_document);
	const std::vector<std::string> types = descs->document_types();
	BOOST_TEST((std::ranges::find(types, "material_desc") != types.end()));

	const std::vector<std::string_view> codecs = desc::schema_generator<scn::material_desc>::codec_fields();
	BOOST_TEST((codecs == std::vector<std::string_view>{ "samplers_textures" }));
	BOOST_TEST(desc::document_problem(*schema, descs->schemas(), codecs).empty());
	BOOST_TEST(!desc::document_problem(*schema, descs->schemas()).empty());
}

BOOST_AUTO_TEST_SUITE_END()
