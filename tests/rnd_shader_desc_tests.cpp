#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/render/rnd_shader_desc.h>
#include <snk/render/rnd_render_service_init.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// The shader program as a desc (#1199): stages and defines, read and written by its schema, loaded like every desc.

namespace json = boost::json;
namespace fs = std::filesystem;

#ifndef SNK_CORE_RES_DIR
#error "SNK_CORE_RES_DIR must point at core/res"
#endif

namespace {

json::object ser(const desc::desc_base& d)
{
	json::object o;
	d.serialize(o);
	return o;
}

std::vector<std::byte> read_bytes(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	return test_util::to_bytes(text);
}

struct shader_desc_fixture : test_util::scene_env
{
	shader_desc_fixture()
		: adapter(*rs, *descs)
	{
		engine::render::register_render_descs(*descs);
	}

	~shader_desc_fixture()
	{
		engine::render::unregister_render_descs(*descs);
	}

	core::desc::desc_adapter_t adapter;
};

constexpr const char* k_shipped[] = { "scene", "sky", "mix_opaque_trans_scene" };

}

BOOST_FIXTURE_TEST_SUITE(RndShaderDescTests, shader_desc_fixture)

BOOST_AUTO_TEST_CASE(RegisteredAsADocumentType)
{
	BOOST_TEST(rnd::shader_desc::__type == "shader_desc");
	const desc::type_schema* schema = descs->find_schema("shader_desc");
	BOOST_REQUIRE(schema);
	BOOST_TEST(schema->is_document);
	BOOST_TEST(descs->current_version("shader_desc") == 1);
}

BOOST_AUTO_TEST_CASE(SerializeThenDeserializeKeepsTheStagesAndTheDefines)
{
	rnd::shader_desc src;
	src.vertex = res::tag{ "res://shaders/obj.vert" };
	src.fragment = res::tag{ "res://shaders/obj.frag" };
	src.geometry = res::tag{ "res://shaders/obj.geom" };
	src.defines = { "USE_NORMAL_MAP", "LIGHTS_ENABLED" };

	const json::object written = ser(src);
	rnd::shader_desc dst;
	dst.deserialize(*descs, written);

	BOOST_TEST(dst.vertex == src.vertex);
	BOOST_TEST(dst.fragment == src.fragment);
	BOOST_TEST(dst.geometry == src.geometry);
	BOOST_TEST(dst.defines == src.defines);
	BOOST_TEST(json::value(ser(dst)) == json::value(written));
}

BOOST_AUTO_TEST_CASE(AnEmptyStageIsWrittenAsNull_InTheOrderOfTheFields)
{
	rnd::shader_desc src;
	src.vertex = res::tag{ "res://shaders/scene.vert" };
	src.fragment = res::tag{ "res://shaders/scene.frag" };
	BOOST_TEST(json::serialize(ser(src))
		== R"({"vertex":"res://shaders/scene.vert","fragment":"res://shaders/scene.frag","geometry":null,"defines":[]})");

	rnd::shader_desc back;
	back.deserialize(*descs, ser(src));
	BOOST_TEST(!back.geometry.is_valid());
	BOOST_TEST(back.defines.empty());
}

BOOST_AUTO_TEST_CASE(ConfigHoldsTheStagesAndTheDefines)
{
	rnd::shader_desc shader;
	shader.vertex = res::tag{ "res://shaders/scene.vert" };
	shader.fragment = res::tag{ "res://shaders/scene.frag" };
	shader.defines = { "USE_NORMAL_MAP" };

	const rnd::shader_config config = shader.config();
	BOOST_TEST(config.cdata.program.get_vertex_shader() == shader.vertex);
	BOOST_TEST(config.cdata.program.get_fragment_shader() == shader.fragment);
	BOOST_TEST(!config.cdata.program.get_geometry_shader().is_valid());
	BOOST_TEST(config.cdata.program.size() == 3);
	BOOST_TEST(config.cdata.defines == shader.defines);
	BOOST_TEST(config.cdata.constants.empty());
}

// Each shipped program loads through the resource system, names stages that exist, and saves to the bytes of its file.
BOOST_AUTO_TEST_CASE(ShippedProgramsLoadAndSaveToTheirOwnBytes)
{
	rs->unregistrate_resolver(res::tag::default_protocol());
	auto& ram_res = rs->registrate_resolver<test_util::ram_resolver>(res::tag::default_protocol());
	const fs::path shaders = fs::path{ SNK_CORE_RES_DIR } / "shaders";
	for (const char* name : k_shipped) {
		const std::string file = std::string{ name } + ".shader.desc";
		ram_res.files[res::tag::make("shaders/" + file)] = read_bytes(shaders / file);
	}

	for (const char* name : k_shipped) {
		const res::tag tag = res::tag::make(std::string{ "shaders/" } + name + ".shader.desc");
		BOOST_TEST_CONTEXT(tag.string()) {
			const auto shader = load<rnd::shader_desc>(tag);
			BOOST_REQUIRE(shader);
			BOOST_TEST(fs::exists(fs::path{ SNK_CORE_RES_DIR } / shader->vertex.relative()));
			BOOST_TEST(fs::exists(fs::path{ SNK_CORE_RES_DIR } / shader->fragment.relative()));
			BOOST_TEST(shader->vertex.extension() == "vert");
			BOOST_TEST(shader->fragment.extension() == "frag");
			BOOST_TEST(!shader->geometry.is_valid());
			BOOST_TEST(shader->defines.empty());

			const std::vector<std::byte> saved = adapter.serialize(tag, shader);
			const std::vector<std::byte>& file = ram_res.files.at(tag);
			BOOST_CHECK_MESSAGE(saved == file, "run tools/desc_resave on core/res");
		}
	}
	const auto mix = load<rnd::shader_desc>(res::tag::make("shaders/mix_opaque_trans_scene.shader.desc"));
	BOOST_REQUIRE(mix);
	BOOST_TEST(mix->vertex.string() == "res://shaders/scene.vert");
	BOOST_TEST(mix->fragment.string() == "res://shaders/mix_opaque_trans_scene.frag");
}

BOOST_AUTO_TEST_CASE(ARewrittenFileReloadsIntoANewConfig)
{
	const res::tag tag = put("hot.shader.desc", R"({"__type":"shader_desc","__version":1,
		"vertex":"res://shaders/scene.vert","fragment":"res://shaders/scene.frag","geometry":null,"defines":[]})");
	const auto before = load<rnd::shader_desc>(tag);
	BOOST_REQUIRE(before);
	const rnd::shader_config old_config = before->config();

	put("hot.shader.desc", R"({"__type":"shader_desc","__version":1,
		"vertex":"res://shaders/scene.vert","fragment":"res://shaders/mix_opaque_trans_scene.frag","geometry":null,
		"defines":["USE_NORMAL_MAP"]})");
	rs->signal_changed(tag);
	pump();
	const auto after = load<rnd::shader_desc>(tag);
	BOOST_REQUIRE(after);
	const rnd::shader_config new_config = after->config();

	BOOST_TEST(!(new_config == old_config));
	BOOST_TEST(new_config.cdata.program.get_fragment_shader().string() == "res://shaders/mix_opaque_trans_scene.frag");
	BOOST_TEST((new_config.cdata.defines == std::vector<std::string>{ "USE_NORMAL_MAP" }));
	BOOST_TEST(old_config.cdata.defines.empty());
}

BOOST_AUTO_TEST_SUITE_END()
