#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/render/rnd_shader_desc.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/desc/desc_json_schema.h>
#include <snk/desc/desc_schema_ops.h>
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

// #1440: a shader says it has a transparent path; the default (none) is not written, so no shipped file changes.
BOOST_AUTO_TEST_CASE(TheTransparentFlagIsWrittenOnlyWhenSet)
{
	rnd::shader_desc src;
	src.vertex = res::tag{ "res://shaders/scene.vert" };
	src.fragment = res::tag{ "res://shaders/scene.frag" };
	BOOST_TEST(!ser(src).contains("transparent"));

	src.transparent = true;
	const json::object written = ser(src);
	BOOST_REQUIRE(written.contains("transparent"));
	BOOST_TEST(written.at("transparent").as_bool());
	rnd::shader_desc back;
	back.deserialize(*descs, written);
	BOOST_TEST(back.transparent);
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
			BOOST_TEST(shader->backends.empty());

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

// An agent is told and checked the same way (#1200): the shader of a material is a reference to a file, never inline.
BOOST_AUTO_TEST_CASE(TheShaderOfAMaterial_IsAReferenceOnly_InTheSchemaAndItsChecks)
{
	const desc::type_schema* material = descs->find_schema("material_desc");
	BOOST_REQUIRE(material);
	const desc::field_schema* shader = material->find_field("/shader");
	BOOST_REQUIRE(shader);
	BOOST_TEST(shader->reference_only);

	const json::object document = desc::to_json_schema(*material, descs->schemas());
	const json::object& property = document.at("properties").as_object().at("shader").as_object();
	BOOST_REQUIRE(property.contains("oneOf"));
	const json::object& as_object = property.at("oneOf").as_array().at(1).as_object();
	BOOST_TEST(as_object.at("additionalProperties").as_bool() == false);
	BOOST_TEST(!as_object.at("properties").as_object().contains("__type"));

	const auto check = [&](std::string_view patch) {
		return desc::check_override_object(*material, json::parse(patch).as_object(), nullptr, &descs->schemas()).status;
	};
	BOOST_TEST((check(R"({"shader":"res://shaders/scene.shader.desc"})") == desc::override_status::valid));
	BOOST_TEST((check(R"({"shader":{"guid":"77f4fc60-9a12-4df0-8471-9a57d3e400db","path":"res://shaders/scene.shader.desc"}})") == desc::override_status::valid));
	BOOST_TEST((check(R"({"shader":{"__type":"shader_desc","vertex":"res://shaders/scene.vert","fragment":"res://shaders/scene.frag"}})")
		== desc::override_status::bad_value));
}

// #1203: a file with per-backend overrides saves to its own bytes: the backends by name, in each only the stages it names.
BOOST_AUTO_TEST_CASE(BackendOverridesSaveToTheBytesOfTheirFile)
{
	rs->unregistrate_resolver(res::tag::default_protocol());
	auto& ram_res = rs->registrate_resolver<test_util::ram_resolver>(res::tag::default_protocol());
	const std::string text =
		"{\n"
		"    \"__type\" : \"shader_desc\",\n"
		"    \"__version\" : 1,\n"
		"    \"vertex\" : \"res://shaders/scene.vert\",\n"
		"    \"fragment\" : \"res://shaders/scene.frag\",\n"
		"    \"defines\" : [],\n"
		"    \"backends\" : {\n"
		"        \"metal\" : {\n"
		"            \"vertex\" : \"res://shaders/metal/scene.vert\",\n"
		"            \"fragment\" : \"res://shaders/metal/scene.frag\"\n"
		"        },\n"
		"        \"vulkan\" : {\n"
		"            \"fragment\" : \"res://shaders/vulkan/scene.frag\"\n"
		"        }\n"
		"    }\n"
		"}\n";
	const res::tag tag = res::tag::make("shaders/over.shader.desc");
	ram_res.files[tag] = test_util::to_bytes(text);

	const auto shader = load<rnd::shader_desc>(tag);
	BOOST_REQUIRE(shader);
	BOOST_REQUIRE_EQUAL(shader->backends.size(), 2u);
	BOOST_TEST(shader->backends.at("metal").vertex.string() == "res://shaders/metal/scene.vert");
	BOOST_TEST(!shader->backends.at("metal").geometry.is_valid());
	BOOST_TEST(!shader->backends.at("vulkan").vertex.is_valid());
	BOOST_TEST(shader->backends.at("vulkan").fragment.string() == "res://shaders/vulkan/scene.frag");

	const std::vector<std::byte> saved = adapter.serialize(tag, shader);
	BOOST_CHECK_MESSAGE(saved == ram_res.files.at(tag), "the file with backends did not save to its own bytes");
}

BOOST_AUTO_TEST_CASE(StagesFor_PutsTheStagesABackendNamesOverTheSharedOnes)
{
	rnd::shader_desc shader;
	shader.vertex = res::tag{ "res://shaders/scene.vert" };
	shader.fragment = res::tag{ "res://shaders/scene.frag" };
	shader.backends["vulkan"] = rnd::shader_stage_sources{ .vertex = res::tag{}, .fragment = res::tag{ "res://shaders/vk.frag" }, .geometry = res::tag{} };
	shader.backends["metal"] = rnd::shader_stage_sources{ .vertex = res::tag{ "res://shaders/mtl.vert" },
		.fragment = res::tag{ "res://shaders/mtl.frag" }, .geometry = res::tag{ "res://shaders/mtl.geom" } };
	shader.backends["webgpu"] = rnd::shader_stage_sources{ .vertex = res::tag{ "res://shaders/w.vert" }, .fragment = res::tag{}, .geometry = res::tag{} };

	const rnd::shader_stage_sources shared = shader.stages_for("opengl");
	BOOST_TEST(shared.vertex == shader.vertex);
	BOOST_TEST(shared.fragment == shader.fragment);
	BOOST_TEST(!shared.geometry.is_valid());

	const rnd::shader_stage_sources partial = shader.stages_for("vulkan");
	BOOST_TEST(partial.vertex == shader.vertex);
	BOOST_TEST(partial.fragment.string() == "res://shaders/vk.frag");
	BOOST_TEST(!partial.geometry.is_valid());

	const rnd::shader_stage_sources full = shader.stages_for("metal");
	BOOST_TEST(full.vertex.string() == "res://shaders/mtl.vert");
	BOOST_TEST(full.fragment.string() == "res://shaders/mtl.frag");
	BOOST_TEST(full.geometry.string() == "res://shaders/mtl.geom");

	// A name outside the known list is still an override; a name no entry has is the shared stages.
	BOOST_TEST(shader.stages_for("webgpu").vertex.string() == "res://shaders/w.vert");
	const rnd::shader_stage_sources unknown = shader.stages_for("d3d12");
	BOOST_TEST(unknown.vertex == shader.vertex);
	BOOST_TEST(unknown.fragment == shader.fragment);
	BOOST_TEST(!unknown.geometry.is_valid());

	// The runtime reads the shared stages only (#1029).
	BOOST_TEST(shader.config().cdata.program.get_fragment_shader() == shader.fragment);
}

BOOST_AUTO_TEST_CASE(ABackendOverride_IsCheckedLikeTheSharedStages_AndAnUnknownOrEmptyOneIsAWarning)
{
	put("over.shader.desc", R"({"__type":"shader_desc","vertex":"res://shaders/a.vert","fragment":"res://shaders/a.frag",
		"backends":{"vulkan":{"fragment":"res://shaders/a.vert"},"webgpu":{"vertex":"res://shaders/w.vert"},"metal":{}}})");
	const auto items = diagnostics_of(tag_of("over.shader.desc"));
	const res::diagnostic* wrong = at(items, "/backends/vulkan/fragment");
	BOOST_REQUIRE_MESSAGE(wrong, "no diagnostic at /backends/vulkan/fragment");
	BOOST_TEST((wrong->severity == res::diagnostic_severity::warning));
	BOOST_TEST(wrong->kind == res::diagnostic_kind::schema);
	BOOST_TEST(wrong->reason.find(".frag") != std::string::npos);
	const res::diagnostic* unknown = at(items, "/backends/webgpu");
	BOOST_REQUIRE_MESSAGE(unknown, "no diagnostic at /backends/webgpu");
	BOOST_TEST(unknown->reason.find("unknown backend 'webgpu'") != std::string::npos);
	BOOST_TEST(!at(items, "/backends/webgpu/vertex"));
	const res::diagnostic* empty = at(items, "/backends/metal");
	BOOST_REQUIRE_MESSAGE(empty, "no diagnostic at /backends/metal");
	BOOST_TEST(empty->reason.find("names no stage") != std::string::npos);
	BOOST_TEST(!at(items, "/vertex"));
	BOOST_TEST(!at(items, "/fragment"));
	BOOST_TEST(items.size() == 3u);
	// The values stay as written.
	const auto shader = load<rnd::shader_desc>(tag_of("over.shader.desc"));
	BOOST_REQUIRE(shader);
	BOOST_TEST(shader->backends.at("webgpu").vertex.string() == "res://shaders/w.vert");
	BOOST_TEST(shader->backends.at("vulkan").fragment.string() == "res://shaders/a.vert");

	put("fine_over.shader.desc", R"({"__type":"shader_desc","vertex":"res://shaders/a.vert","fragment":"res://shaders/a.frag",
		"backends":{"opengl":{"geometry":"res://shaders/a.geom"},"d3d12":{"vertex":"res://shaders/d.vert","fragment":"res://shaders/d.frag"}}})");
	BOOST_TEST(diagnostics_of(tag_of("fine_over.shader.desc")).empty());
}

BOOST_AUTO_TEST_CASE(TheOverridesAreAMapOfARegisteredStructure)
{
	const desc::type_schema* schema = descs->find_schema("shader_desc");
	BOOST_REQUIRE(schema);
	BOOST_TEST(schema->is_document);
	const desc::field_schema* backends = schema->find_field("/backends");
	BOOST_REQUIRE(backends);
	BOOST_TEST((backends->kind == desc::field_kind::map));
	BOOST_TEST((backends->element == desc::field_kind::structure));
	BOOST_TEST(backends->ref_type == "shader_stage_sources");
	const desc::type_schema* stages = descs->find_schema("shader_stage_sources");
	BOOST_REQUIRE(stages);
	BOOST_TEST((stages->kind == desc::schema_kind::structure));
	BOOST_TEST(stages->fields.size() == 3u);
}

// The schema drops a key the structure does not have, and a save would lose it: the load says so.
BOOST_AUTO_TEST_CASE(AKeyOfAnOverrideThatIsNoStage_IsAWarningAtItsKey)
{
	put("typo.shader.desc", R"({"__type":"shader_desc","vertex":"res://shaders/a.vert","fragment":"res://shaders/a.frag",
		"backends":{"vulkan":{"fragmnet":"res://shaders/v.frag","fragment":"res://shaders/v.frag"},"metal":{"compute":"res://shaders/m.comp"}}})");
	const auto items = diagnostics_of(tag_of("typo.shader.desc"));
	const res::diagnostic* typo = at(items, "/backends/vulkan/fragmnet");
	BOOST_REQUIRE_MESSAGE(typo, "no diagnostic at /backends/vulkan/fragmnet");
	BOOST_TEST((typo->severity == res::diagnostic_severity::warning));
	BOOST_TEST(typo->kind == res::diagnostic_kind::schema);
	BOOST_TEST(typo->reason == "unknown stage 'fragmnet' in the override of 'vulkan'");
	const res::diagnostic* compute = at(items, "/backends/metal/compute");
	BOOST_REQUIRE_MESSAGE(compute, "no diagnostic at /backends/metal/compute");
	BOOST_TEST(compute->reason == "unknown stage 'compute' in the override of 'metal'");
	// With its only key unknown, the metal override names no stage either.
	BOOST_TEST(at(items, "/backends/metal"));
	BOOST_TEST(!at(items, "/backends/vulkan"));
	BOOST_TEST(!at(items, "/backends/vulkan/fragment"));
}

// A null is no map: the schema's own warning at the key, and the overrides stay empty.
BOOST_AUTO_TEST_CASE(NullBackends_LoadAsNone_WithTheSchemasWarning)
{
	put("null.shader.desc", R"({"__type":"shader_desc","vertex":"res://shaders/a.vert","fragment":"res://shaders/a.frag","backends":null})");
	const auto shader = load<rnd::shader_desc>(tag_of("null.shader.desc"));
	BOOST_REQUIRE(shader);
	BOOST_TEST(shader->backends.empty());
	BOOST_TEST(!ser(*shader).contains("backends"));
	const auto items = diagnostics_of(tag_of("null.shader.desc"));
	const res::diagnostic* null_value = at(items, "/backends");
	BOOST_REQUIRE_MESSAGE(null_value, "no diagnostic at /backends");
	BOOST_TEST(null_value->reason.find("does not fit") != std::string::npos, null_value->reason);
	BOOST_TEST(items.size() == 1u);
}

BOOST_AUTO_TEST_CASE(EmptyBackends_AreLeftOutOnSave)
{
	put("empty.shader.desc", R"({"__type":"shader_desc","vertex":"res://shaders/a.vert","fragment":"res://shaders/a.frag","backends":{}})");
	const auto shader = load<rnd::shader_desc>(tag_of("empty.shader.desc"));
	BOOST_REQUIRE(shader);
	BOOST_TEST(shader->backends.empty());
	BOOST_TEST(!ser(*shader).contains("backends"));
	BOOST_TEST(diagnostics_of(tag_of("empty.shader.desc")).empty());
}

// A backend name is a JSON object key: "/" and "~" are escaped in the warnings' pointers, and the name round-trips.
BOOST_AUTO_TEST_CASE(ABackendNameWithSlashAndTilde_IsEscapedInPointers_AndRoundTrips)
{
	put("escaped.shader.desc", R"({"__type":"shader_desc","vertex":"res://shaders/a.vert","fragment":"res://shaders/a.frag",
		"backends":{"a/b~c":{"fragment":"res://shaders/x.vert","x/y~z":"res://shaders/x.frag"}}})");
	const auto items = diagnostics_of(tag_of("escaped.shader.desc"));
	const res::diagnostic* unknown = at(items, "/backends/a~1b~0c");
	BOOST_REQUIRE_MESSAGE(unknown, "no diagnostic at /backends/a~1b~0c");
	BOOST_TEST(unknown->reason == "unknown backend 'a/b~c'");
	BOOST_TEST(at(items, "/backends/a~1b~0c/fragment"));
	const res::diagnostic* stage = at(items, "/backends/a~1b~0c/x~1y~0z");
	BOOST_REQUIRE_MESSAGE(stage, "no diagnostic at /backends/a~1b~0c/x~1y~0z");
	BOOST_TEST(stage->reason == "unknown stage 'x/y~z' in the override of 'a/b~c'");

	const auto shader = load<rnd::shader_desc>(tag_of("escaped.shader.desc"));
	BOOST_REQUIRE(shader);
	BOOST_TEST(shader->stages_for("a/b~c").fragment.string() == "res://shaders/x.vert");
	const json::object written = ser(*shader);
	BOOST_TEST(written.at("backends").as_object().contains("a/b~c"));
	rnd::shader_desc back;
	back.deserialize(*descs, written);
	BOOST_TEST(json::value(ser(back)) == json::value(written));
}

BOOST_AUTO_TEST_CASE(ABackendNameThatDiffersOnlyInCase_GetsAHint)
{
	put("case.shader.desc", R"({"__type":"shader_desc","vertex":"res://shaders/a.vert","fragment":"res://shaders/a.frag",
		"backends":{"Vulkan":{"fragment":"res://shaders/v.frag"}}})");
	const auto items = diagnostics_of(tag_of("case.shader.desc"));
	const res::diagnostic* named = at(items, "/backends/Vulkan");
	BOOST_REQUIRE_MESSAGE(named, "no diagnostic at /backends/Vulkan");
	BOOST_TEST(named->reason == "unknown backend 'Vulkan': did you mean 'vulkan'?");
	const auto shader = load<rnd::shader_desc>(tag_of("case.shader.desc"));
	BOOST_REQUIRE(shader);
	BOOST_TEST(shader->stages_for("vulkan").fragment.string() == "res://shaders/a.frag");
}

BOOST_AUTO_TEST_SUITE_END()
