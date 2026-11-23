#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/render/rnd_shader_desc.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/scene/scn_material_desc.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The shader program as a desc (#1199): stages and defines, read and written by its schema, loaded like every desc; the
// material refers to one (#1200).

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

	// The diagnostics of one request of `tag`, after it and what it started have settled.
	std::vector<res::diagnostic> diagnostics_of(const res::tag& tag)
	{
		auto handle = require_loaded<desc::desc_base>(tag);
		res::diagnostics_collector collector;
		collector.collect(handle.get_control_block());
		return collector.items();
	}

	static const res::diagnostic* at(const std::vector<res::diagnostic>& items, std::string_view path)
	{
		const auto it = std::ranges::find_if(items, [&](const res::diagnostic& d) { return d.json_path == path; });
		return it == items.end() ? nullptr : &*it;
	}

	static rnd::shader_desc program(std::vector<std::string> defines)
	{
		rnd::shader_desc shader;
		shader.vertex = res::tag{ "res://shaders/scene.vert" };
		shader.fragment = res::tag{ "res://shaders/scene.frag" };
		shader.defines = std::move(defines);
		return shader;
	}

	core::desc::desc_adapter_t adapter;
};

// The IDs of the shipped programs: materials and projects refer to them by these (EPIC-57), so they never change.
constexpr std::pair<const char*, const char*> k_shipped_guids[] = {
	{ "scene", "77f4fc60-9a12-4df0-8471-9a57d3e400db" },
	{ "sky", "e0df66d4-e01f-43a6-81e4-c035dc7efd6a" },
	{ "mix_opaque_trans_scene", "ecb283a7-19cf-40bd-a394-675b63d0b67c" },
};

std::string guid_of(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	boost::system::error_code ec;
	const json::value parsed = json::parse(text, ec);
	if (ec || !parsed.is_object()) return {};
	const json::value* guid = parsed.as_object().if_contains("__guid");
	return guid && guid->is_string() ? std::string{ guid->as_string() } : std::string{};
}

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

BOOST_AUTO_TEST_CASE(AnEmptyOptionalStageIsLeftOut_InTheOrderOfTheFields)
{
	rnd::shader_desc src;
	src.vertex = res::tag{ "res://shaders/scene.vert" };
	src.fragment = res::tag{ "res://shaders/scene.frag" };
	BOOST_TEST(json::serialize(ser(src))
		== R"({"vertex":"res://shaders/scene.vert","fragment":"res://shaders/scene.frag","defines":[]})");

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
		"vertex":"res://shaders/scene.vert","fragment":"res://shaders/scene.frag","defines":[]})");
	const auto before = load<rnd::shader_desc>(tag);
	BOOST_REQUIRE(before);
	const rnd::shader_config old_config = before->config();

	put("hot.shader.desc", R"({"__type":"shader_desc","__version":1,
		"vertex":"res://shaders/scene.vert","fragment":"res://shaders/mix_opaque_trans_scene.frag",
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

BOOST_AUTO_TEST_CASE(TheShippedProgramsKeepTheirIds)
{
	const fs::path shaders = fs::path{ SNK_CORE_RES_DIR } / "shaders";
	for (const auto& [name, guid] : k_shipped_guids) {
		BOOST_TEST_CONTEXT(name) {
			const std::string file = std::string{ name } + ".shader.desc";
			BOOST_TEST(guid_of(shaders / file) == guid);
			BOOST_TEST(guid_of(shaders / (file + ".resource")) == guid);
		}
	}
}

// #1200: the material's defines join the shader's: sorted, each once, so one set of defines is one variant.
BOOST_AUTO_TEST_CASE(ConfigMergesTheExtraDefines_SortedWithoutDuplicates)
{
	const rnd::shader_config config = program({ "USE_NORMAL_MAP", "LIGHTS 4" }).config({ "USE_SPECULAR_MAP", "USE_NORMAL_MAP" });
	BOOST_TEST((config.cdata.defines == std::vector<std::string>{ "LIGHTS 4", "USE_NORMAL_MAP", "USE_SPECULAR_MAP" }));

	BOOST_TEST((program({ "B", "A", "B" }).config().cdata.defines == std::vector<std::string>{ "A", "B" }));
	BOOST_TEST(program({}).config().cdata.defines.empty());
}

BOOST_AUTO_TEST_CASE(ConfigKeepsTheMaterialsDefine_WhenBothNameTheSameMacro)
{
	const rnd::shader_config config = program({ "LIGHTS 4", "USE_NORMAL_MAP" }).config({ "LIGHTS 8" });
	BOOST_TEST((config.cdata.defines == std::vector<std::string>{ "LIGHTS 8", "USE_NORMAL_MAP" }));

	// "LIGHTS" alone and "LIGHTS 8" define the same macro too.
	BOOST_TEST((program({ "LIGHTS" }).config({ "LIGHTS 8" }).cdata.defines == std::vector<std::string>{ "LIGHTS 8" }));
	BOOST_TEST((program({ "LIGHTS 8" }).config({ "LIGHTS" }).cdata.defines == std::vector<std::string>{ "LIGHTS" }));
	// A longer name that starts with the same letters is another macro.
	BOOST_TEST((program({ "LIGHTS_ON" }).config({ "LIGHTS 8" }).cdata.defines == std::vector<std::string>{ "LIGHTS 8", "LIGHTS_ON" }));
}

// operator== of constant_data reads the defines in order, variant_hash does not: the merge makes them agree.
BOOST_AUTO_TEST_CASE(ConfigOfTheSameDefinesIsEqualWhateverTheirOrderAndSide)
{
	const rnd::shader_config::constant_data a = program({ "B", "A" }).config().cdata;
	const rnd::shader_config::constant_data b = program({ "A" }).config({ "B" }).cdata;
	const rnd::shader_config::constant_data c = program({}).config({ "B", "A", "A" }).cdata;
	BOOST_CHECK(a == b);
	BOOST_CHECK(a == c);
	BOOST_TEST(a.variant_hash() == b.variant_hash());
	BOOST_TEST(a.variant_hash() == c.variant_hash());
}

BOOST_AUTO_TEST_CASE(AStageThatIsMissingOrOfAnotherKind_IsAWarningAtItsKey)
{
	put("no_vertex.shader.desc", R"({"__type":"shader_desc","fragment":"res://shaders/a.frag"})");
	const auto missing = diagnostics_of(tag_of("no_vertex.shader.desc"));
	const res::diagnostic* vertex = at(missing, "/vertex");
	BOOST_REQUIRE_MESSAGE(vertex, "no diagnostic at /vertex");
	BOOST_TEST((vertex->severity == res::diagnostic_severity::warning));
	BOOST_TEST(vertex->kind == res::diagnostic_kind::schema);
	BOOST_TEST(vertex->reason.find("missing") != std::string::npos);
	BOOST_TEST(!at(missing, "/fragment"));

	put("swapped.shader.desc", R"({"__type":"shader_desc","vertex":"res://shaders/a.frag","fragment":"res://shaders/a.frag",
		"geometry":"res://shaders/a.vert"})");
	const auto swapped = diagnostics_of(tag_of("swapped.shader.desc"));
	const res::diagnostic* wrong = at(swapped, "/vertex");
	BOOST_REQUIRE_MESSAGE(wrong, "no diagnostic at /vertex");
	BOOST_TEST(wrong->reason.find("res://shaders/a.frag") != std::string::npos);
	BOOST_TEST(wrong->reason.find(".vert") != std::string::npos);
	BOOST_TEST(at(swapped, "/geometry"));
	BOOST_TEST(!at(swapped, "/fragment"));
	// The values stay as written: the file says what it says.
	const auto shader = load<rnd::shader_desc>(tag_of("swapped.shader.desc"));
	BOOST_REQUIRE(shader);
	BOOST_TEST(shader->vertex.string() == "res://shaders/a.frag");

	put("fine.shader.desc", R"({"__type":"shader_desc","vertex":"res://shaders/a.vert","fragment":"res://shaders/a.frag",
		"geometry":"res://shaders/a.geom"})");
	BOOST_TEST(diagnostics_of(tag_of("fine.shader.desc")).empty());
}

// #1200: a material refers to a program by its tag or a {guid, path} reference; an inline program is refused.
BOOST_AUTO_TEST_CASE(AMaterialRefersToAProgram)
{
	put("ref.shader.desc", R"({"__type":"shader_desc","vertex":"res://shaders/scene.vert","fragment":"res://shaders/scene.frag"})");
	put("by_tag.mat.desc", R"({"__type":"material_desc","shader":"memory://t/ref.shader.desc"})");
	put("by_reference.mat.desc", R"({"__type":"material_desc","shader":{"path":"memory://t/ref.shader.desc"}})");
	for (const char* name : { "by_tag.mat.desc", "by_reference.mat.desc" }) {
		BOOST_TEST_CONTEXT(name) {
			const auto material = load<scn::material_desc>(tag_of(name));
			BOOST_REQUIRE(material);
			BOOST_TEST(material->shader.get_tag().string() == "memory://t/ref.shader.desc");
			BOOST_REQUIRE(material->shader.is_ready());
			BOOST_TEST(material->shader.get()->fragment.string() == "res://shaders/scene.frag");
			BOOST_TEST(!at(diagnostics_of(tag_of(name)), "/shader"));
		}
	}
	const auto by_tag = load<scn::material_desc>(tag_of("by_tag.mat.desc"));
	BOOST_REQUIRE(by_tag);
	BOOST_TEST(ser(*by_tag).at("shader").as_string() == "memory://t/ref.shader.desc");
}

// Emptied instead, the material would be saved without its shader and lose it for good.
BOOST_AUTO_TEST_CASE(AnInlineProgramInAMaterial_RefusesTheLoad)
{
	put("inline.mat.desc", R"({"__type":"material_desc","queue":"transparent","shader":{"__type":"shader_desc",
		"vertex":"res://shaders/scene.vert","fragment":"res://shaders/scene.frag"}})");
	BOOST_TEST(!load<scn::material_desc>(tag_of("inline.mat.desc")));

	const auto items = diagnostics_of(tag_of("inline.mat.desc"));
	const res::diagnostic* refused = at(items, "/shader");
	BOOST_REQUIRE_MESSAGE(refused, "no diagnostic at /shader");
	BOOST_TEST((refused->severity == res::diagnostic_severity::error));
	BOOST_TEST(refused->kind == res::diagnostic_kind::reference);
	BOOST_TEST(refused->reason.find("inline shader_desc") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
