#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "rnd_null_driver.h"
#include "shader/rnd_shader_manager.h"
#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/render/rnd_gpu_blocks.h>
#include <string>

// What the engine does to a shader source before the compiler sees it: includes, defines after the #version line,
// the binding macros of the backend. A source it cannot prepare is not loaded, instead of being handed on with a
// macro the compiler cannot read.

namespace {

using namespace rnd;

struct shader_load_fixture
{
	res::tag put(std::string_view name, std::string_view content) { return env.put(name, content); }

	shader_config::constant_data program_of(const res::tag& vertex, const res::tag& fragment)
	{
		shader_config::constant_data cdata;
		cdata.program = shader_config::shader_program_data::build().set_vertex_shader(vertex).set_fragment_shader(fragment);
		return cdata;
	}

	shader_config::load_result load(const shader_config::constant_data& cdata)
	{
		return shader_config::load(cdata, *env.rs, driver::per_type_binding_table);
	}

	test_util::ram_res_env env;
};

constexpr const char* good_fragment = "#version 450 core\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n";

}

BOOST_FIXTURE_TEST_SUITE(RndShaderLoadTests, shader_load_fixture)

BOOST_AUTO_TEST_CASE(DefinesFollowTheVersionLineEvenWhenCommentsComeFirst)
{
	const auto vertex = put("lic.vert", "// Copyright\n\n/* a block\n#version 100\n*/\n#version 450 core\nSNK_UBO(0, 0) uniform M { mat4 p; };\nvoid main() { gl_Position = p[0]; }\n");
	const auto fragment = put("lic.frag", good_fragment);
	auto cdata = program_of(vertex, fragment);
	cdata.defines.push_back("MY_DEFINE");

	const auto result = load(cdata);
	BOOST_REQUIRE_MESSAGE(result.ok(), result.error);
	BOOST_REQUIRE(result.headers.size() == 2u);
	const std::string& body = result.headers[0].body;
	const auto version = body.find("\n#version 450 core\n");
	BOOST_REQUIRE(version != std::string::npos);
	BOOST_TEST(body.rfind("// Copyright", 0) == 0u);	// what was before the directive is still there
	const std::string define_light = "#define MAX_LIGHT_COUNT " + std::to_string(lights_params::MAX_LIGHT_COUNT) + "\n";
	BOOST_TEST(body.find(define_light) == version + std::string{ "\n#version 450 core\n" }.size());
	BOOST_TEST(body.find("#define SNK_VERTEX_SHADER\n") > version);
	BOOST_TEST(body.find("#define MY_DEFINE\n") > version);
	// The macro was expanded with the backend's table.
	BOOST_TEST(body.find("layout(std140, binding = 0) uniform M") != std::string::npos);
	BOOST_TEST(body.find("SNK_UBO") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(AShaderWithAMisspeltMacroIsNotLoaded)
{
	const auto vertex = put("typo.vert", "#version 450 core\nSNK_UBOO(0, 0) uniform M { mat4 p; };\nvoid main() {}\n");
	const auto result = load(program_of(vertex, put("typo.frag", good_fragment)));
	BOOST_TEST(!result.ok());
	BOOST_TEST(result.headers.empty());
	BOOST_TEST(result.error.find("typo.vert") != std::string::npos);
	BOOST_TEST(result.error.find("line 2: SNK_UBOO") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AMacroOutsideTheBindingTableOrMalformedIsNotLoaded)
{
	const auto outside = load(program_of(put("outside.vert", "#version 450 core\nSNK_UBO(0, 2) uniform M { mat4 p; };\nvoid main() {}\n"), put("outside.frag", good_fragment)));
	BOOST_TEST(!outside.ok());
	BOOST_TEST(outside.error.find("SNK_UBO(0, 2) is outside the binding table") != std::string::npos);

	const auto malformed = load(program_of(put("malformed.vert", "#version 450 core\nSNK_TEX uniform sampler2D t;\nvoid main() {}\n"), put("malformed.frag", good_fragment)));
	BOOST_TEST(!malformed.ok());
	BOOST_TEST(malformed.error.find("SNK_TEX needs (set, slot)") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AMacroInACommentDoesNotStopTheLoad)
{
	const auto vertex = put("commented.vert", "#version 450 core\n/* SNK_UBO(9, 9) uniform Gone {}; */\n// SNK_FOO(1)\nvoid main() {}\n");
	const auto result = load(program_of(vertex, put("commented.frag", good_fragment)));
	BOOST_TEST_REQUIRE(result.ok());
	BOOST_TEST(result.headers[0].body.find("/* SNK_UBO(9, 9) uniform Gone {}; */") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ASourceWithoutAVersionDirectiveIsNotLoaded)
{
	const auto no_version = load(program_of(put("plain.vert", "void main() {}\n"), put("plain.frag", good_fragment)));
	BOOST_TEST(!no_version.ok());
	BOOST_TEST(no_version.error.find("plain.vert") != std::string::npos);
	BOOST_TEST(no_version.error.find("#version") != std::string::npos);

	const auto late = load(program_of(put("late.vert", "int x;\n#version 450 core\nvoid main() {}\n"), put("late.frag", good_fragment)));
	BOOST_TEST(!late.ok());
}

namespace {
	std::size_t count_of(const std::string& text, std::string_view needle)
	{
		std::size_t count = 0;
		for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
			++count;
		}
		return count;
	}
}

BOOST_AUTO_TEST_CASE(AFileIncludedTwiceIsExpandedOnce)
{
	put("shared.glsl", "float shared_marker;\n");
	put("mid.glsl", "#include \"shared.glsl\"\nfloat mid_marker;\n");
	const auto vertex = put("twice.vert", "#version 450 core\n#include \"shared.glsl\"\n#include \"mid.glsl\"\n#include \"shared.glsl\"\nvoid main() {}\n");
	const auto result = load(program_of(vertex, put("twice.frag", good_fragment)));
	BOOST_REQUIRE_MESSAGE(result.ok(), result.error);
	const std::string& body = result.headers[0].body;
	BOOST_TEST(count_of(body, "float shared_marker;") == 1u);
	BOOST_TEST(count_of(body, "float mid_marker;") == 1u);
	BOOST_TEST(body.find("#include") == std::string::npos);
	BOOST_TEST(body.find("void main()") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AnIncludeCycleIsReportedAndStopsTheLoad)
{
	put("ping.glsl", "#include \"pong.glsl\"\nfloat ping;\n");
	put("pong.glsl", "#include \"ping.glsl\"\nfloat pong;\n");
	const auto vertex = put("cycle.vert", "#version 450 core\n#include \"ping.glsl\"\nvoid main() {}\n");
	const auto result = load(program_of(vertex, put("cycle.frag", good_fragment)));
	BOOST_TEST(!result.ok());
	BOOST_TEST(result.headers.empty());
	BOOST_TEST(result.error.find("include cycle") != std::string::npos);
	BOOST_TEST(result.error.find("ping.glsl -> ") != std::string::npos);
	BOOST_TEST(result.error.find("pong.glsl") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AShaderThatIncludesItselfIsReportedAsACycle)
{
	const auto vertex = put("self.vert", "#version 450 core\n#include \"self.vert\"\nvoid main() {}\n");
	const auto result = load(program_of(vertex, put("self.frag", good_fragment)));
	BOOST_TEST(!result.ok());
	BOOST_TEST(result.error.find("include cycle") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AMissingIncludeIsReportedWithItsLine)
{
	const auto vertex = put("missing.vert", "#version 450 core\n\n#include \"not_there.glsl\"\nvoid main() {}\n");
	const auto result = load(program_of(vertex, put("missing.frag", good_fragment)));
	BOOST_TEST(!result.ok());
	BOOST_TEST(result.headers.empty());
	BOOST_TEST(result.error.find("missing.vert' line 3") != std::string::npos);
	BOOST_TEST(result.error.find("cannot read include \"not_there.glsl\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AnIncludeWithoutAQuotedNameIsReportedAndOneInACommentIsIgnored)
{
	put("lib.glsl", "float lib_marker;\n");
	const auto bad = load(program_of(put("angle.vert", "#version 450 core\n#include <lib.glsl>\nvoid main() {}\n"), put("angle.frag", good_fragment)));
	BOOST_TEST(!bad.ok());
	BOOST_TEST(bad.error.find("needs a file name in double quotes") != std::string::npos);

	const auto commented = load(program_of(put("quiet.vert", "#version 450 core\n// #include \"nowhere.glsl\"\n/*\n#include \"lib.glsl\"\n*/\nvoid main() {}\n"), put("quiet.frag", good_fragment)));
	BOOST_REQUIRE_MESSAGE(commented.ok(), commented.error);
	BOOST_TEST(commented.headers[0].body.find("lib_marker") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheNumberOfLightsDoesNotAddADefineToAProgram)
{
	auto cdata = program_of(put("lit.vert", "#version 450 core\nvoid main() {}\n"), put("lit.frag", good_fragment));
	cdata.defines.push_back("LIGHTS_ENABLED");
	const auto result = load(cdata);
	BOOST_REQUIRE_MESSAGE(result.ok(), result.error);
	const std::string& body = result.headers[0].body;
	// The array length is the one fixed constant of the engine; counts live in the GlobalLights block.
	BOOST_TEST(count_of(body, "LIGHT_COUNT") == 1u);
	BOOST_TEST(body.find("#define MAX_LIGHT_COUNT " + std::to_string(lights_params::MAX_LIGHT_COUNT) + "\n") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AProgramWithoutAnyReadableStageIsNotLoaded)
{
	const auto result = load(program_of(res::tag{ "memory://t/nowhere.vert" }, res::tag{ "memory://t/nowhere.frag" }));
	BOOST_TEST(!result.ok());
	BOOST_TEST(result.headers.empty());
}

BOOST_AUTO_TEST_CASE(TheShaderManagerNeverHandsABadSourceToTheDriver)
{
	driver::null::driver drv;
	drv.record_shader_sources(true);
	ShaderManager shaders(&drv, *env.rs);

	const auto bad = shaders.variants().intern(program_of(put("bad.vert", "#version 450 core\nSNK_SSBO(1, 0) buffer B { uint d[]; };\nvoid main() {}\n"), put("bad.frag", good_fragment)));
	BOOST_TEST(shaders.program(bad) == nullptr);
	BOOST_TEST(shaders.program(bad) == nullptr);	// asked again at the next draw: still no program, and no new attempt to compile
	BOOST_TEST(drv.shader_sources().empty());
	BOOST_TEST(drv.stats().shaders.created == 0u);

	const auto good = shaders.variants().intern(program_of(put("ok.vert", "#version 450 core\nvoid main() {}\n"), put("ok.frag", good_fragment)));
	driver::shader_interface* program = shaders.program(good);
	BOOST_REQUIRE(program);
	BOOST_TEST(shaders.program(good) == program);
	BOOST_TEST(drv.shader_sources().size() == 1u);
	BOOST_TEST(drv.stats().shaders.created == 1u);
}

BOOST_AUTO_TEST_SUITE_END()
