#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "rnd_null_driver.h"
#include "shader/rnd_shader_manager.h"
#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/render/rnd_shader_problem.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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

// Where a GLSL compiler puts the line of `source` that holds `marker`: the source string and line number it reports. A
// small stand-in for its preprocessor: #define, #ifdef, #ifndef, #else and #endif decide which groups are taken, and a
// #line counts only in a taken group (every other #if is taken as true).
struct compiler_location
{
	std::size_t source = 0;
	int line = 0;
};

std::optional<compiler_location> compiler_location_of(std::string_view source, std::string_view marker, std::size_t* active_hits = nullptr)
{
	std::optional<compiler_location> first;
	struct group
	{
		bool active = true;
		bool parent = true;
		bool taken = false;
	};
	std::vector<group> groups;
	std::set<std::string> defined;
	compiler_location at{ 0, 1 };
	const auto word_after = [](std::string_view text, std::string_view directive) -> std::optional<std::string_view> {
		const std::size_t first = text.find_first_not_of(" \t");
		if (first == std::string_view::npos || text[first] != '#') return std::nullopt;
		text.remove_prefix(text.find_first_not_of(" \t", first + 1) == std::string_view::npos ? text.size() : text.find_first_not_of(" \t", first + 1));
		if (!text.starts_with(directive)) return std::nullopt;
		text.remove_prefix(directive.size());
		if (!text.empty() && text.front() != ' ' && text.front() != '\t' && text.front() != '\r') return std::nullopt;
		const std::size_t begin = text.find_first_not_of(" \t");
		if (begin == std::string_view::npos) return std::string_view{};
		const std::size_t end = text.find_first_of(" \t\r", begin);
		return text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
	};
	std::size_t line_begin = 0;
	while (line_begin < source.size()) {
		const std::size_t newline = source.find('\n', line_begin);
		const std::string_view line = source.substr(line_begin, newline == std::string_view::npos ? std::string_view::npos : newline - line_begin);
		line_begin = newline == std::string_view::npos ? source.size() : newline + 1;
		const bool active = groups.empty() || groups.back().active;
		if (line.find(marker) != std::string_view::npos && active) {
			if (!active_hits) {
				return at;
			}
			++*active_hits;
			if (!first) {
				first = at;
			}
		}
		if (const auto ifdef = word_after(line, "ifdef")) {
			const bool on = active && defined.contains(std::string{ *ifdef });
			groups.push_back({ on, active, on });
		} else if (const auto ifndef = word_after(line, "ifndef")) {
			const bool on = active && !defined.contains(std::string{ *ifndef });
			groups.push_back({ on, active, on });
		} else if (word_after(line, "if")) {
			groups.push_back({ active, active, active });
		} else if (word_after(line, "else") && !groups.empty()) {
			groups.back().active = groups.back().parent && !groups.back().taken;
		} else if (word_after(line, "endif") && !groups.empty()) {
			groups.pop_back();
		} else if (const auto define = word_after(line, "define"); define && active) {
			defined.insert(std::string{ *define });
		} else if (const auto number = word_after(line, "line"); number && active) {
			const std::string_view rest = line.substr(line.find(*number) + number->size());
			const std::size_t k = rest.find_first_not_of(" \t");
			at.line = std::stoi(std::string{ *number });
			if (k != std::string_view::npos && std::isdigit(static_cast<unsigned char>(rest[k]))) {
				at.source = static_cast<std::size_t>(std::stoi(std::string{ rest.substr(k) }));
			}
			continue;
		}
		++at.line;
	}
	return first;
}

// How many lines with `marker` a GLSL preprocessor keeps (compiler_location_of's groups).
std::size_t active_count(std::string_view source, std::string_view marker)
{
	std::size_t hits = 0;
	compiler_location_of(source, marker, &hits);
	return hits;
}

// The include guard macro that comes first in `body` (#1301).
std::string first_guard(const std::string& body)
{
	const std::size_t at = body.find("#ifndef SNK_INCLUDE_");
	if (at == std::string::npos) {
		return {};
	}
	const std::size_t begin = at + std::string_view{ "#ifndef " }.size();
	return body.substr(begin, body.find_first_of("\r\n", begin) - begin);
}

driver::shader_build_log stage_log(std::size_t stage, std::string text)
{
	driver::shader_build_log log;
	log.stages.push_back({ stage, std::move(text) });
	return log;
}

}

BOOST_FIXTURE_TEST_SUITE(RndShaderProblemsTests, shader_problems_fixture)

BOOST_AUTO_TEST_CASE(ACheckReportsTheProblemsOfAProgramWithoutTouchingTheCache)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	drv.judge_shaders_with([](const std::vector<driver::shader_header>& headers) -> std::optional<driver::shader_build_log> {
		if (headers[0].body.find("BROKEN") == std::string::npos) {
			return std::nullopt;
		}
		return stage_log(0, "1(1) : error C1008: undefined variable \"BROKEN\"\n");
	});
	const auto lib = put("check_lib.glsl", "BROKEN;\n");
	const auto vertex = put("check.vert", "#version 450 core\n#include \"check_lib.glsl\"\nvoid main() {}\n");
	const auto fragment = put("check.frag", good_fragment);
	const std::uint64_t generation = shaders.problems_generation();
	const std::size_t variants = shaders.variants().size();

	const shader_check_result result = shaders.check(program_of(vertex, fragment));
	BOOST_REQUIRE(result.problems.size() == 1u);
	BOOST_TEST(result.problems[0].file == lib);
	BOOST_TEST(result.problems[0].line == 1);
	BOOST_TEST((std::ranges::find(result.files, lib) != result.files.end()));
	BOOST_TEST((std::ranges::find(result.files, vertex) != result.files.end()));
	BOOST_TEST(shaders.problems().empty());
	BOOST_TEST(shaders.problems_generation() == generation);
	BOOST_TEST(shaders.variants().size() == variants);
}

BOOST_AUTO_TEST_CASE(ACheckOfAGoodProgramHasNoProblems_AndKeepsNoProgram)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	const std::size_t variants = shaders.variants().size();
	const shader_check_result result = shaders.check(program_of(put("ok.vert", "#version 450 core\nvoid main() {}\n"), put("ok.frag", good_fragment)));
	BOOST_TEST(result.problems.empty());
	BOOST_TEST(result.files.size() == 2u);
	BOOST_TEST(shaders.variants().size() == variants);
	BOOST_TEST(shaders.problems().empty());
}

BOOST_AUTO_TEST_CASE(ACheckOfASourceThatCannotBePrepared_IsAProblem_AndIsNotLoggedAsAnError)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	const shader_check_result result = shaders.check(program_of(put("nov.vert", "void main() {}\n"), put("nov.frag", good_fragment)));
	BOOST_REQUIRE(!result.problems.empty());
	BOOST_TEST(result.problems[0].code == "shader/load");
	const auto quiet = shader_config::load(program_of(put("nov2.vert", "void main() {}\n"), put("nov2.frag", good_fragment)), *env.rs,
		driver::per_type_binding_table, shader_config::load_options{ true });
	BOOST_TEST(!quiet.ok());
}

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
	const std::string guard = first_guard(body);
	BOOST_REQUIRE(guard.size() == std::string_view{ "SNK_INCLUDE_" }.size() + 16u);
	BOOST_TEST(body.ends_with(std::format("#define SNK_VERTEX_SHADER\n#line 2 0\n#ifndef {0}\n#define {0}\n#line 1 1\nfloat a;\nfloat b;\n#endif\n"
		"#line 3 0\nvoid main() {{}}\n", guard)), body);
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
	const std::string& body = result.headers[0].body;
	const std::string guard = first_guard(body);
	BOOST_REQUIRE(!guard.empty());
	// Both includes are there, under the same guard and the same source number: the compiler keeps the first copy only.
	BOOST_TEST(body.ends_with(std::format("#ifndef {0}\n#define {0}\n#line 1 1\nfloat a;\n#endif\n#line 3 0\n"
		"#ifndef {0}\n#define {0}\n#line 1 1\nfloat a;\n#endif\n#line 4 0\nvoid main() {{}}\n", guard)), body);
	BOOST_TEST(result.sources[0].size() == 2u);
	BOOST_TEST(active_count(body, "float a;") == 1u);
}

// #1301: an include first met in a group the compiler skips used to hide the file from a later include in a group it keeps.
BOOST_AUTO_TEST_CASE(AnIncludeInASkippedGroupDoesNotHideTheFileFromALaterInclude)
{
	const auto lib = put("guarded_lib.glsl", "float lib_fn() { return 1.0; }\n");
	const auto vertex = put("guarded.vert", "#version 450 core\n#ifdef NOT_DEFINED\n#include \"guarded_lib.glsl\"\n#endif\n"
		"#include \"guarded_lib.glsl\"\nvoid main() { lib_fn(); }\n");
	const auto result = load(program_of(vertex, put("guarded.frag", good_fragment)));
	BOOST_REQUIRE_MESSAGE(result.ok(), result.error);
	const std::string& body = result.headers[0].body;
	BOOST_TEST(active_count(body, "float lib_fn()") == 1u, body);
	const auto at = compiler_location_of(body, "float lib_fn()");
	BOOST_REQUIRE(at);
	BOOST_TEST(at->source == 1u);
	BOOST_TEST(at->line == 1);
	BOOST_TEST((result.sources[0] == std::vector<res::tag>{ vertex, lib }));
	const auto main_at = compiler_location_of(body, "void main()");
	BOOST_REQUIRE(main_at);
	BOOST_TEST(main_at->source == 0u);
	BOOST_TEST(main_at->line == 6);
}

// Every include is expanded now, so a file included twice by each of many nested files would grow without end.
BOOST_AUTO_TEST_CASE(IncludesThatExpandPastTheLimitAreAProblem)
{
	constexpr int levels = 24;
	const std::string filler(1024, ' ');
	for (int i = 0; i < levels; ++i) {
		const std::string next = std::format("grow{}.glsl", i + 1);
		put(std::format("grow{}.glsl", i), std::format("#include \"{0}\"\n#include \"{0}\"\n//{1}\n", next, filler));
	}
	put(std::format("grow{}.glsl", levels), "float leaf;\n//" + filler + "\n");
	const auto vertex = put("grow.vert", "#version 450 core\n#include \"grow0.glsl\"\nvoid main() {}\n");

	const auto started = std::chrono::steady_clock::now();
	const auto result = load(program_of(vertex, put("grow.frag", good_fragment)));
	const bool within_ten_seconds = std::chrono::steady_clock::now() - started < std::chrono::seconds(10);
	BOOST_TEST(within_ten_seconds);
	BOOST_TEST(!result.ok());
	std::size_t too_large = 0;
	for (const shader_problem& problem : result.problems) {
		if (problem.message.find("the includes expand to more than") != std::string::npos) {
			++too_large;
			BOOST_TEST(problem.code == "shader/include");
		}
	}
	BOOST_TEST(too_large == 1u);
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

// Fable's case: an include in a group the compiler skips. Its #line directives are not followed there, so the numbering is
// set again after the #endif. The fake compiler reports the marker where a GLSL preprocessor puts it (compiler_location_of).
BOOST_AUTO_TEST_CASE(AnIncludeInASkippedGroupDoesNotMoveTheLinesAfterIt)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	put("skipped_lib.glsl", "float a;\nfloat b;\nfloat c;\n");
	const auto vertex = put("skipped.vert", "#version 450 core\n#ifdef NOT_DEFINED\n#include \"skipped_lib.glsl\"\n#endif\nERROR_HERE;\nvoid main() {}\n");
	const auto variant = shaders.variants().intern(program_of(vertex, put("skipped.frag", good_fragment)));
	drv.judge_shaders_with([](const std::vector<driver::shader_header>& headers) -> std::optional<driver::shader_build_log> {
		const auto at = compiler_location_of(headers[0].body, "ERROR_HERE");
		if (!at) {
			return std::nullopt;
		}
		return stage_log(0, std::format("{}({}) : error C1008: undefined variable \"ERROR_HERE\"\n", at->source, at->line));
	});
	BOOST_TEST(shaders.program(variant) == nullptr);
	const auto problems = shaders.problems();
	BOOST_REQUIRE(problems.size() == 1u);
	BOOST_TEST(problems[0].file == vertex);
	BOOST_TEST(problems[0].line == 5);
}

BOOST_AUTO_TEST_CASE(EveryGroupEndOrTurnSetsTheNumberingAgain)
{
	put("turn_lib.glsl", "#ifdef A\nfloat a;\n#else\nfloat b;\n#endif\n");
	const auto vertex = put("turn.vert", "#version 450 core\n#ifdef B\n#include \"turn_lib.glsl\"\n#endif\nvoid main() {}\n");
	const auto result = load(program_of(vertex, put("turn.frag", good_fragment)));
	BOOST_REQUIRE_MESSAGE(result.ok(), result.error);
	const std::string& body = result.headers[0].body;
	const std::string guard = first_guard(body);
	BOOST_REQUIRE(!guard.empty());
	BOOST_TEST(body.ends_with(std::format("#ifdef B\n#ifndef {0}\n#define {0}\n#line 1 1\n#ifdef A\nfloat a;\n#else\n#line 4 1\nfloat b;\n#endif\n"
		"#line 6 1\n#endif\n#line 4 0\n#endif\n#line 5 0\nvoid main() {{}}\n", guard)), body);
}

BOOST_AUTO_TEST_CASE(ASourceNumberOutsideTheTableIsTheStageWithoutALine)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	const auto vertex = put("unknown_source.vert", "#version 450 core\nvoid main() {}\n");
	const auto variant = shaders.variants().intern(program_of(vertex, put("unknown_source.frag", good_fragment)));
	drv.judge_shaders_with([](const std::vector<driver::shader_header>&) -> std::optional<driver::shader_build_log> {
		return stage_log(0, "7(12) : error C0000: somewhere\n");
	});
	BOOST_TEST(shaders.program(variant) == nullptr);
	const auto problems = shaders.problems();
	BOOST_REQUIRE(problems.size() == 1u);
	BOOST_TEST(problems[0].file == vertex);
	BOOST_TEST(problems[0].line == 0);
}

BOOST_AUTO_TEST_CASE(AHugeLineAndAFloodOfErrorsAreBounded)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	const auto huge = shaders.variants().intern(program_of(put("huge.vert", "#version 450 core\nvoid main() {}\n"), put("huge.frag", good_fragment)));
	const auto flood = shaders.variants().intern(program_of(put("flood.vert", "#version 450 core\nvoid main() {}\n"), put("flood.frag", good_fragment)));
	std::string one_line = "0(2) : error C0000: " + std::string(200 * 1024, 'x') + "\n";
	std::string many_lines;
	for (int i = 1; i <= 10000; ++i) {
		many_lines += std::format("0({}) : error C0000: e\n", i);
	}
	drv.judge_shaders_with([&](const std::vector<driver::shader_header>& headers) -> std::optional<driver::shader_build_log> {
		return stage_log(0, headers[0].title.starts_with("huge") ? one_line : many_lines);
	});

	const auto started = std::chrono::steady_clock::now();
	BOOST_TEST(shaders.program(huge) == nullptr);
	BOOST_TEST(shaders.program(flood) == nullptr);
	// BOOST_TEST would print the time_point operand, which has no operator<<.
	const bool within_ten_seconds = std::chrono::steady_clock::now() - started < std::chrono::seconds(10);
	BOOST_TEST(within_ten_seconds);

	const auto problems = shaders.problems();
	BOOST_REQUIRE(problems.size() == 1u + 101u);
	BOOST_TEST(problems[0].line == 2);
	BOOST_TEST(problems[0].message.size() <= 4u * 1024u + 3u);
	BOOST_TEST(problems[0].message.ends_with("\xE2\x80\xA6"));
	BOOST_TEST(problems[100].line == 100);
	BOOST_TEST(problems[101].line == 0);
	BOOST_TEST(problems[101].message == "\xE2\x80\xA6 9900 more problems");
}

BOOST_AUTO_TEST_CASE(ALineNumberOfMoreThanNineDigitsIsNoLine)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	const auto vertex = put("long_number.vert", "#version 450 core\nvoid main() {}\n");
	const auto variant = shaders.variants().intern(program_of(vertex, put("long_number.frag", good_fragment)));
	drv.judge_shaders_with([](const std::vector<driver::shader_header>&) -> std::optional<driver::shader_build_log> {
		return stage_log(0, "0:123456789012(5): error: too far\n0:0000000002(3): error: padded\n");
	});
	BOOST_TEST(shaders.program(variant) == nullptr);
	const auto problems = shaders.problems();
	BOOST_REQUIRE(problems.size() == 2u);
	BOOST_TEST(problems[0].file == vertex);
	BOOST_TEST(problems[0].line == 0);
	BOOST_TEST(problems[0].message.find("123456789012") != std::string::npos);
	BOOST_TEST(problems[1].line == 2);
	BOOST_TEST(problems[1].column == 3);
}

BOOST_AUTO_TEST_CASE(ALineCutInsideAMultibyteCharacterStaysValidUtf8)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	const auto variant = shaders.variants().intern(program_of(put("utf8_cut.vert", "#version 450 core\nvoid main() {}\n"), put("utf8_cut.frag", good_fragment)));
	const std::string prefix = "0(2) : error C0000: ";
	// Byte 4096 of the line is the second byte of an "\xC3\xA9" pair.
	std::string line = prefix + std::string(4 * 1024 - 1 - prefix.size(), 'x');
	for (int i = 0; i < 100; ++i) {
		line += "\xC3\xA9";
	}
	drv.judge_shaders_with([&line](const std::vector<driver::shader_header>&) -> std::optional<driver::shader_build_log> {
		return stage_log(0, line + "\n");
	});
	BOOST_TEST(shaders.program(variant) == nullptr);
	const auto problems = shaders.problems();
	BOOST_REQUIRE(problems.size() == 1u);
	const std::string_view ellipsis = "\xE2\x80\xA6";
	BOOST_REQUIRE(problems[0].message.ends_with(ellipsis));
	const std::string_view kept = std::string_view{ problems[0].message }.substr(0, problems[0].message.size() - ellipsis.size());
	BOOST_TEST(kept.ends_with('x'));
}

BOOST_AUTO_TEST_SUITE_END()
