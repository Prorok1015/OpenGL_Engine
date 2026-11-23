#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "rnd_null_driver.h"
#include "shader/rnd_shader_manager.h"
#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/render/rnd_shader_problem.h>
#include <optional>
#include <string>
#include <vector>

// #1201: why a shader program did not build, at the file and line: the engine's preprocessing errors, the #line
// directives that make the compiler report the lines of the files themselves, the logs of the drivers mapped back
// through them, and what ShaderManager keeps of it across hot reloads.

namespace {

using namespace rnd;

constexpr const char* good_fragment = "#version 450 core\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n";

struct shader_problems_fixture
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

driver::shader_build_log stage_log(std::size_t stage, std::string text)
{
	driver::shader_build_log log;
	log.stages.push_back({ stage, std::move(text) });
	return log;
}

}

BOOST_FIXTURE_TEST_SUITE(RndShaderProblemsTests, shader_problems_fixture)

BOOST_AUTO_TEST_CASE(AnIncludeThatCannotBeReadIsAProblemOfTheIncludingFileAtItsLine)
{
	const auto vertex = put("missing.vert", "#version 450 core\n\n#include \"nowhere.glsl\"\nvoid main() {}\n");
	const auto result = load(program_of(vertex, put("missing.frag", good_fragment)));
	BOOST_TEST(!result.ok());
	BOOST_TEST(result.error.find("cannot read include") != std::string::npos);	// the log line stays
	BOOST_REQUIRE(result.problems.size() == 1u);
	const shader_problem& problem = result.problems[0];
	BOOST_TEST(problem.code == "shader/include");
	BOOST_TEST(problem.file == vertex);
	BOOST_TEST(problem.line == 3);
	BOOST_TEST(problem.message.find("cannot read include \"nowhere.glsl\"") != std::string::npos);
	BOOST_TEST(problem.program == "missing.vert + missing.frag");
}

BOOST_AUTO_TEST_CASE(ABadMacroInsideAnIncludedFileIsAProblemOfThatFileAtItsLine)
{
	const auto lib = put("macro_lib.glsl", "float a;\nSNK_UBOO(0, 0) uniform M { mat4 p; };\n");
	const auto vertex = put("macro.vert", "#version 450 core\n#include \"macro_lib.glsl\"\nfloat b;\nSNK_FOO(1, 2) uniform N { mat4 q; };\nvoid main() {}\n");
	const auto result = load(program_of(vertex, put("macro.frag", good_fragment)));
	BOOST_TEST(!result.ok());
	BOOST_REQUIRE(result.problems.size() == 2u);
	BOOST_TEST(result.problems[0].code == "shader/macro");
	BOOST_TEST(result.problems[0].file == lib);
	BOOST_TEST(result.problems[0].line == 2);
	BOOST_TEST(result.problems[0].message.find("SNK_UBOO is not a binding macro") == 0u);
	BOOST_TEST(result.problems[1].code == "shader/macro");
	BOOST_TEST(result.problems[1].file == vertex);
	BOOST_TEST(result.problems[1].line == 4);
}

BOOST_AUTO_TEST_CASE(ASourceWithoutVersionIsALoadProblemOfItsStage)
{
	const auto vertex = put("noversion.vert", "void main() {}\n");
	const auto result = load(program_of(vertex, put("noversion.frag", good_fragment)));
	BOOST_REQUIRE(result.problems.size() == 1u);
	BOOST_TEST(result.problems[0].code == "shader/load");
	BOOST_TEST(result.problems[0].file == vertex);
}

BOOST_AUTO_TEST_CASE(TheExpandedSourceNumbersItsLinesAsTheFilesDo)
{
	const auto lib = put("layout_lib.glsl", "float a;\nfloat b;\n");
	const auto vertex = put("layout.vert", "#version 450 core\n#include \"layout_lib.glsl\"\nvoid main() {}\n");
	const auto fragment = put("layout.frag", good_fragment);
	const auto result = load(program_of(vertex, fragment));
	BOOST_REQUIRE_MESSAGE(result.ok(), result.error);
	const std::string& body = result.headers[0].body;
	BOOST_TEST(body.starts_with("#version 450 core\n#define MAX_LIGHT_COUNT "));
	BOOST_TEST(body.ends_with("#define SNK_VERTEX_SHADER\n#line 2 0\n#line 1 1\nfloat a;\nfloat b;\n#line 3 0\nvoid main() {}\n"), body);
	BOOST_REQUIRE(result.sources.size() == 2u);
	BOOST_TEST((result.sources[0] == std::vector<res::tag>{ vertex, lib }));
	BOOST_TEST((result.sources[1] == std::vector<res::tag>{ fragment }));

	// The directive follows the defines wherever the #version line is.
	const auto late = load(program_of(put("late_version.vert", "// a comment\n\n#version 450 core\nvoid main() {}\n"), put("late_version.frag", good_fragment)));
	BOOST_REQUIRE_MESSAGE(late.ok(), late.error);
	BOOST_TEST(late.headers[0].body.ends_with("#define SNK_VERTEX_SHADER\n#line 4 0\nvoid main() {}\n"), late.headers[0].body);
}

BOOST_AUTO_TEST_CASE(AFileIncludedTwiceKeepsTheLinesAfterItsSecondInclude)
{
	put("twice_lib.glsl", "float a;\n");
	const auto vertex = put("twice_lines.vert", "#version 450 core\n#include \"twice_lib.glsl\"\n#include \"twice_lib.glsl\"\nvoid main() {}\n");
	const auto result = load(program_of(vertex, put("twice_lines.frag", good_fragment)));
	BOOST_REQUIRE_MESSAGE(result.ok(), result.error);
	BOOST_TEST(result.headers[0].body.ends_with("#line 1 1\nfloat a;\n#line 3 0\n\nvoid main() {}\n"), result.headers[0].body);
}

BOOST_AUTO_TEST_CASE(TheLogsOfTheThreeVendorsMapToTheFilesAndLines)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	const auto lib = put("vendor_lib.glsl", "float a;\nfloat b;\n");
	const auto vertex = put("vendor.vert", "#version 450 core\n#include \"vendor_lib.glsl\"\nfloat c;\nfloat d;\nvoid main() {}\n");
	const auto fragment = put("vendor.frag", good_fragment);
	const auto variant = shaders.variants().intern(program_of(vertex, fragment));

	drv.judge_shaders_with([](const std::vector<driver::shader_header>&) -> std::optional<driver::shader_build_log> {
		driver::shader_build_log log = stage_log(0,
			"1(2) : error C0000: syntax error, unexpected identifier\n"
			"0:3(5): error: `x' undeclared\n"
			"ERROR: 0:4: 'y' : undeclared identifier\n"
			"ERROR: 3 compilation errors.  No code generated.\n");
		log.stages.push_back({ 1, "something the parser does not know\n" });
		return log;
	});
	BOOST_TEST(shaders.program(variant) == nullptr);
	const std::vector<shader_problem> problems = shaders.problems();
	BOOST_REQUIRE(problems.size() == 4u);
	BOOST_TEST(problems[0].code == "shader/compile");
	BOOST_TEST(problems[0].file == lib);	// NVIDIA, inside the include
	BOOST_TEST(problems[0].line == 2);
	BOOST_TEST(problems[0].column == 0);
	BOOST_TEST(problems[0].message == "error C0000: syntax error, unexpected identifier");
	BOOST_TEST(problems[1].file == vertex);	// Mesa
	BOOST_TEST(problems[1].line == 3);
	BOOST_TEST(problems[1].column == 5);
	BOOST_TEST(problems[1].message == "error: `x' undeclared");
	BOOST_TEST(problems[2].file == vertex);	// AMD, the summary line left out
	BOOST_TEST(problems[2].line == 4);
	BOOST_TEST(problems[2].message == "'y' : undeclared identifier");
	BOOST_TEST(problems[3].file == fragment);	// a line in no known form
	BOOST_TEST(problems[3].line == 0);
	BOOST_TEST(problems[3].message == "something the parser does not know");
	for (const shader_problem& problem : problems) {
		BOOST_TEST(problem.program == "vendor.vert + vendor.frag");
	}
}

BOOST_AUTO_TEST_CASE(ALinkLogIsOneProblemOfTheVertexStage)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	const auto vertex = put("link.vert", "#version 450 core\nvoid main() {}\n");
	const auto variant = shaders.variants().intern(program_of(vertex, put("link.frag", good_fragment)));
	drv.judge_shaders_with([](const std::vector<driver::shader_header>&) -> std::optional<driver::shader_build_log> {
		driver::shader_build_log log;
		log.link = "error: fragment shader input 'v' has no matching output\n";
		return log;
	});
	BOOST_TEST(shaders.program(variant) == nullptr);
	const auto problems = shaders.problems();
	BOOST_REQUIRE(problems.size() == 1u);
	BOOST_TEST(problems[0].code == "shader/link");
	BOOST_TEST(problems[0].file == vertex);
	BOOST_TEST(problems[0].line == 0);
	BOOST_TEST(problems[0].message == "error: fragment shader input 'v' has no matching output");
}

BOOST_AUTO_TEST_CASE(TheNullDriverPutsItsReasonIntoTheLog)
{
	driver::null::driver drv;
	driver::shader_build_log log;
	BOOST_TEST(!drv.create_shader({ driver::shader_header{ "a.vert", "void main() {}\n", driver::shader_header::shader_type::VERTEX } }, &log));
	BOOST_REQUIRE(log.stages.size() == 1u);
	BOOST_TEST(log.stages[0].stage == 0u);
	BOOST_TEST(log.stages[0].text.find("#version") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AFailedRebuildKeepsTheProgramAndReportsAndAFixClearsTheProblems)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	drv.judge_shaders_with([](const std::vector<driver::shader_header>& headers) -> std::optional<driver::shader_build_log> {
		const std::size_t at = headers[0].body.find("BROKEN");
		if (at == std::string::npos) {
			return std::nullopt;
		}
		return stage_log(0, "1(1) : error C1008: undefined variable \"BROKEN\"\n");
	});
	const auto lib = put("reload_lib.glsl", "float ok;\n");
	const auto variant = shaders.variants().intern(program_of(put("reload.vert", "#version 450 core\n#include \"reload_lib.glsl\"\nvoid main() {}\n"), put("reload.frag", good_fragment)));

	driver::shader_interface* const good = shaders.program(variant);
	BOOST_REQUIRE(good);
	BOOST_TEST(shaders.problems().empty());
	const std::uint64_t clean = shaders.problems_generation();

	put("reload_lib.glsl", "BROKEN;\n");
	env.rs->signal_changed(lib);
	env.pump();
	BOOST_TEST(shaders.program(variant) == good);
	const auto problems = shaders.problems();
	BOOST_REQUIRE(problems.size() == 1u);
	BOOST_TEST(problems[0].file == lib);
	BOOST_TEST(problems[0].line == 1);
	const std::uint64_t broken = shaders.problems_generation();
	BOOST_TEST(broken > clean);
	BOOST_TEST(shaders.program(variant) == good);
	BOOST_TEST(shaders.problems_generation() == broken);	// asked again without a change: not built again

	put("reload_lib.glsl", "float fixed_again;\n");
	env.rs->signal_changed(lib);
	env.pump();
	BOOST_TEST(shaders.program(variant) != nullptr);
	BOOST_TEST(shaders.program(variant) != good);
	BOOST_TEST(shaders.problems().empty());
	BOOST_TEST(shaders.problems_generation() > broken);
}

BOOST_AUTO_TEST_CASE(APreprocessingErrorReachesTheManagerAndAFixClearsIt)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	const auto vertex = put("pre.vert", "#version 450 core\n#include \"pre_lib.glsl\"\nvoid main() {}\n");
	const auto variant = shaders.variants().intern(program_of(vertex, put("pre.frag", good_fragment)));
	BOOST_TEST(shaders.program(variant) == nullptr);
	auto problems = shaders.problems();
	BOOST_REQUIRE(problems.size() == 1u);
	BOOST_TEST(problems[0].code == "shader/include");
	BOOST_TEST(problems[0].file == vertex);
	BOOST_TEST(problems[0].line == 2);

	const auto lib = put("pre_lib.glsl", "float late;\n");
	env.rs->signal_changed(lib);
	env.pump();
	BOOST_TEST(shaders.program(variant) != nullptr);
	BOOST_TEST(shaders.problems().empty());
}

BOOST_AUTO_TEST_SUITE_END()
