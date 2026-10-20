#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include <snk/scene/scn_material_desc.h>
#include <snk/render/rnd_state_helper.h>
#include <snk/resource/res_tag_json.h>
#include <snk/scene/scn_glm_json_convert.h>

namespace json = boost::json;

BOOST_AUTO_TEST_SUITE(ScnMaterialDescTests)

BOOST_AUTO_TEST_CASE(MaterialDesc_TypeConstant)
{
	BOOST_TEST(scn::material_desc::__type == "material_desc");
}

// material_desc::deserialize only hands its desc_system argument on to the schema reader as the context of desc_ref
// fields, and material_desc has none (it only reads JSON into POD members after EPIC-49 US-49-5). Constructing a real desc_system here is impossible — the project's
// tests deliberately avoid resource_system, which asserts on a real resources path. So we pass a
// reference that is never dereferenced. Keep deserialize free of desc_system use or this breaks.
static desc::desc_system& shared_desc_system()
{
	// Aligned, non-null storage (never constructed or dereferenced): binding a reference to null trips UBSan.
	// (desc_system is an incomplete type here, so use max-aligned dummy bytes.)
	alignas(std::max_align_t) static unsigned char dummy_bytes[64];
	return *reinterpret_cast<desc::desc_system*>(dummy_bytes);
}

BOOST_AUTO_TEST_CASE(MaterialDesc_RoundTrip_ShaderTagsAndScalars)
{
	scn::material_desc src;
	src.vertex_shader = res::tag{ "shd://obj.vert" };
	src.fragment_shader = res::tag{ "shd://obj.frag" };
	src.geometry_shader = res::tag{ "shd://obj.geom" };
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

	BOOST_TEST(dst.vertex_shader == src.vertex_shader);
	BOOST_TEST(dst.fragment_shader == src.fragment_shader);
	BOOST_TEST(dst.geometry_shader == src.geometry_shader);
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
