#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "rnd_null_driver.h"
#include "shader/rnd_shader_manager.h"
#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/render/rnd_shader_problem.h>
#include <snk/resource/res_asset_index.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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

// One ID per file and any spelling of it, as the editor's asset database answers (res_asset_index_tests' fake_index).
// Locked: the file watcher's thread asks it too.
class watch_index : public res::asset_index
{
public:
	std::optional<res::asset_id> id_of(const res::tag& tag) const override
	{
		std::lock_guard lock(m_mutex);
		const auto it = m_ids.find(tag);
		return it == m_ids.end() ? std::nullopt : std::optional{ it->second };
	}

	std::optional<res::tag> path_of(const res::asset_id& id) const override
	{
		std::lock_guard lock(m_mutex);
		const auto it = m_paths.find(id);
		return it == m_paths.end() ? std::nullopt : std::optional{ it->second };
	}

	void put(const res::tag& tag, const res::asset_id& id)
	{
		std::lock_guard lock(m_mutex);
		m_ids[tag] = id;
		m_paths[id] = tag;
	}

	void spell(const res::tag& spelling, const res::asset_id& id)
	{
		std::lock_guard lock(m_mutex);
		m_ids[spelling] = id;
	}

	void move(const res::tag& from, const res::tag& to)
	{
		std::lock_guard lock(m_mutex);
		const res::asset_id id = m_ids.at(from);
		m_ids.erase(from);
		m_ids[to] = id;
		m_paths[id] = to;
	}

private:
	mutable std::mutex m_mutex;
	std::unordered_map<res::tag, res::asset_id> m_ids;
	std::unordered_map<res::asset_id, res::tag> m_paths;
};

const res::asset_id k_stage_id{ 0x6f1c0b5200004000ull, 0x8000000000001387ull };

// res:// on disk that reports its changes and allows rename: the batches apply_asset_batch moves a watcher with.
struct shader_disk_fixture
{
	test_util::temp_dir dir{ "rnd_shader_problems_disk" };
	ds::task_system tasks{ test_util::manual_tasks() };
	std::shared_ptr<watch_index> index = std::make_shared<watch_index>();
	std::unique_ptr<res::resource_system> rs;

	shader_disk_fixture()
	{
		dir.write("res/rename.vert", "#version 450 core\nvoid main() {}\n");
		dir.write("res/rename.frag", good_fragment);
		res::watch_options watch;
		watch.report_changes = true;
		rs = std::make_unique<res::resource_system>(tasks, res::mount_table{
			res::mount{ res::tag::default_protocol(), { dir.path() / "res" }, res::mount_access::read_write,
				res::write_policy::allow_all(), std::move(watch), true },
		});
		rs->set_asset_index(index);
	}

	~shader_disk_fixture()
	{
		rs.reset();
	}

	void pump() { test_util::pump(tasks); }
};

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
	BOOST_TEST(body.starts_with("#version 450 core\n#line 1 2\n#define MAX_LIGHT_COUNT "));	// the defines: one past the files
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

BOOST_AUTO_TEST_CASE(AProgramTitleParsesBackToTheDefinesItWasFormattedFrom)
{
	const std::vector<std::string> entries{ "USE_FOG", "LEVEL 2", "OPAQUE" };
	const std::string title = "m.vert + m.frag " + format_program_defines(entries);
	BOOST_TEST(title == "m.vert + m.frag { USE_FOG | LEVEL 2 | OPAQUE }");
	BOOST_TEST((parse_program_defines(title) == entries));
	BOOST_TEST(format_program_defines({}).empty());
	BOOST_TEST(parse_program_defines("m.vert + m.frag").empty());

	// The title load() gives a problem names the constants and the defines it was built from (constants in any order).
	shader_config::constant_data cdata = program_of(put("titled.vert", "void main() {}\n"), put("titled.frag", good_fragment));
	cdata.constants = { { "K_ONE", "1" }, { "K_TWO", "2" } };
	cdata.defines = { "USE_FOG", "LEVEL 2" };
	const auto result = load(cdata);
	BOOST_REQUIRE(!result.problems.empty());
	std::vector<std::string> parsed = parse_program_defines(result.problems.front().program);
	std::vector<std::string> expected = program_define_entries(cdata);
	std::ranges::sort(parsed);
	std::ranges::sort(expected);
	BOOST_TEST((parsed == expected));
}

BOOST_AUTO_TEST_CASE(AHostileProgramTitle_GivesTheLastPairsDefines_AndTheTextBeforeIt)
{
	using entries = std::vector<std::string>;
	// "{ }": the " }" overlaps the "{ ", there is no pair.
	BOOST_TEST(parse_program_defines("{ }").empty());
	BOOST_TEST(program_title_stages("{ }") == "{ }");
	// "{  }": a pair with nothing between.
	BOOST_TEST(parse_program_defines("{  }").empty());
	BOOST_TEST(program_title_stages("{  }") == "");
	BOOST_TEST(parse_program_defines("{ A").empty());
	BOOST_TEST(program_title_stages("{ A") == "{ A");
	BOOST_TEST(parse_program_defines("A }").empty());
	BOOST_TEST(program_title_stages("A }") == "A }");
	// The last " }" and the last "{ " before it: the inner "{ " opens, the entry runs to the outer " }".
	BOOST_TEST((parse_program_defines("{ { A } }") == entries{ "A }" }));
	BOOST_TEST(program_title_stages("{ { A } }") == "{");

	std::string flood;
	flood.reserve(100 * 1024);
	while (flood.size() < 100 * 1024) flood += "{ ";
	BOOST_TEST(parse_program_defines(flood).empty());
	BOOST_TEST(program_title_stages(flood).size() == flood.size() - 1);

	const std::string_view odd_path = "res://we{ ird/a.vert + res://b.frag { X | Y }";
	BOOST_TEST((parse_program_defines(odd_path) == entries{ "X", "Y" }));
	BOOST_TEST(program_title_stages(odd_path) == "res://we{ ird/a.vert + res://b.frag");
}

BOOST_AUTO_TEST_CASE(TheEngineDefinesOfAStageAreTheNamedConstants)
{
	const auto result = load(program_of(put("engine.vert", "#version 450 core\nvoid main() {}\n"), put("engine.frag", good_fragment)));
	BOOST_REQUIRE(result.ok());
	BOOST_REQUIRE(result.headers.size() == 2u);
	const std::string light = std::format("#define {} ", max_light_count_define);
	BOOST_TEST(result.headers[0].body.find(light) != std::string::npos);
	BOOST_TEST(result.headers[0].body.find(std::format("#define {}\n", vertex_stage_define)) != std::string::npos);
	BOOST_TEST(result.headers[1].body.find(light) != std::string::npos);
	BOOST_TEST(result.headers[1].body.find(std::format("#define {}\n", fragment_stage_define)) != std::string::npos);
	for (const std::string_view name : { max_light_count_define, vertex_stage_define, fragment_stage_define, geometry_stage_define }) {
		BOOST_TEST(is_engine_define(name));
	}
	BOOST_TEST(!is_engine_define("USE_FOG"));
}

// #1387: the defines after #version are source N of the stage, one past its files. A log line that names N is a problem
// of the stage file with no line that names the define and where it came from; a number past N is no source at all.
BOOST_AUTO_TEST_CASE(AnErrorInAnInjectedDefineNamesTheDefineInEveryVendorForm)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	const auto vertex = put("injected.vert", "#version 450 core\nvoid main() {}\n");
	auto cdata = program_of(vertex, put("injected.frag", good_fragment));
	cdata.constants = { { "K_ONE", "1" } };
	cdata.defines = { "USE_FOG" };
	const auto variant = shaders.variants().intern(cdata);
	drv.judge_shaders_with([](const std::vector<driver::shader_header>&) -> std::optional<driver::shader_build_log> {
		return stage_log(0,
			"1(1) : error C0000: engine one\n"
			"1:3(9): error: constant one\n"
			"ERROR: 1:4: 'USE_FOG' : define one\n"
			"2(1) : error C0000: past the defines\n");
	});
	BOOST_TEST(shaders.program(variant) == nullptr);
	const auto problems = shaders.problems();
	BOOST_REQUIRE(problems.size() == 4u);
	for (const shader_problem& problem : problems) {
		BOOST_TEST(problem.file == vertex);
		BOOST_TEST(problem.line == 0);
		BOOST_TEST(problem.column == 0);
		BOOST_TEST(problem.code == "shader/compile");
	}
	BOOST_TEST(problems[0].message.starts_with(std::format("injected define #1 \"#define {} ", max_light_count_define)), problems[0].message);	// NVIDIA
	BOOST_TEST(problems[0].message.ends_with("\" (engine): error C0000: engine one"), problems[0].message);
	BOOST_TEST(problems[1].message == "injected define #3 \"#define K_ONE 1\" (constant): error: constant one");	// Mesa
	BOOST_TEST(problems[2].message == "injected define #4 \"#define USE_FOG\" (define): 'USE_FOG' : define one");	// AMD
	BOOST_TEST(problems[3].message == "error C0000: past the defines");	// N + 1: the unknown number, as before
}

// Where a GLSL preprocessor puts a constant's line and the stage line right after the defines (compiler_location_of).
BOOST_AUTO_TEST_CASE(AConstantsLineIsTheDefineAndTheStageLineAfterTheDefinesKeepsItsNumber)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	const auto vertex = put("after_defines.vert", "#version 450 core\nAFTER_DEFINES;\nvoid main() {}\n");
	auto cdata = program_of(vertex, put("after_defines.frag", good_fragment));
	cdata.constants = { { "K_MARK", "INJECTED_MARK" } };
	cdata.defines = { "USE_FOG" };
	const auto variant = shaders.variants().intern(cdata);
	drv.judge_shaders_with([](const std::vector<driver::shader_header>& headers) -> std::optional<driver::shader_build_log> {
		const auto injected = compiler_location_of(headers[0].body, "INJECTED_MARK");
		const auto after = compiler_location_of(headers[0].body, "AFTER_DEFINES");
		if (!injected || !after) {
			return std::nullopt;
		}
		return stage_log(0, std::format("{}:{}(1): error: bad constant\n{}:{}(1): error: after\n", injected->source, injected->line,
			after->source, after->line));
	});
	BOOST_TEST(shaders.program(variant) == nullptr);
	const auto problems = shaders.problems();
	BOOST_REQUIRE(problems.size() == 2u);
	BOOST_TEST(problems[0].file == vertex);
	BOOST_TEST(problems[0].line == 0);
	BOOST_TEST(problems[0].message == "injected define #3 \"#define K_MARK INJECTED_MARK\" (constant): error: bad constant");
	BOOST_TEST(problems[1].file == vertex);
	BOOST_TEST(problems[1].line == 2);
	BOOST_TEST(problems[1].message == "error: after");
}

BOOST_AUTO_TEST_CASE(AnErrorInANestedIncludeIsALineOfTheInnermostFile)
{
	driver::null::driver drv;
	ShaderManager shaders(&drv, *env.rs);
	const auto inner = put("nested_b.glsl", "float b1;\nINNER_ERROR;\n");
	const auto outer = put("nested_a.glsl", "float a1;\n#include \"nested_b.glsl\"\nOUTER_ERROR;\n");
	const auto vertex = put("nested.vert", "#version 450 core\n#include \"nested_a.glsl\"\nvoid main() {}\n");
	const auto variant = shaders.variants().intern(program_of(vertex, put("nested.frag", good_fragment)));
	drv.judge_shaders_with([](const std::vector<driver::shader_header>& headers) -> std::optional<driver::shader_build_log> {
		const auto in_b = compiler_location_of(headers[0].body, "INNER_ERROR");
		const auto in_a = compiler_location_of(headers[0].body, "OUTER_ERROR");
		if (!in_b || !in_a) {
			return std::nullopt;
		}
		return stage_log(0, std::format("{}({}) : error C1008: inner\n{}({}) : error C1008: outer\n", in_b->source, in_b->line, in_a->source, in_a->line));
	});
	BOOST_TEST(shaders.program(variant) == nullptr);
	const auto problems = shaders.problems();
	BOOST_REQUIRE(problems.size() == 2u);
	BOOST_TEST(problems[0].file == inner);
	BOOST_TEST(problems[0].line == 2);
	BOOST_TEST(problems[1].file == outer);	// the line after the inner include, numbered as its file again
	BOOST_TEST(problems[1].line == 3);
}

// A watcher is told the path it watched, also when the signal came through another spelling of the file's ID.
BOOST_AUTO_TEST_CASE(AStageWatchedByPathIsBuiltAgainWhenItsIdIsSignalled)
{
	const auto index = std::make_shared<watch_index>();
	const auto vertex = put("by_id.vert", "#version 450 core\nvoid main() {}\n");
	const auto spelled = put("BY_ID.vert", "#version 450 core\nvoid main() {}\n");
	index->put(vertex, k_stage_id);
	index->spell(spelled, k_stage_id);
	env.rs->set_asset_index(index);
	{
		driver::null::driver drv;
		ShaderManager shaders(&drv, *env.rs);
		const auto variant = shaders.variants().intern(program_of(vertex, put("by_id.frag", good_fragment)));
		driver::shader_interface* const first = shaders.program(variant);
		BOOST_REQUIRE(first);

		env.rs->signal_changed(spelled);
		env.pump();
		BOOST_TEST(shaders.program(variant) != nullptr);
		BOOST_TEST(shaders.program(variant) != first);
	}
	env.rs->set_asset_index(nullptr);
}

// After a rename the watcher follows the file and is told its new path; the variants made of it are still found.
BOOST_FIXTURE_TEST_CASE(AStageRenamedThroughTheIndexIsBuiltAgainWhenItChanges, shader_disk_fixture)
{
	const res::tag vertex{ "res://rename.vert" };
	const res::tag fragment{ "res://rename.frag" };
	const res::tag renamed{ "res://renamed.vert" };
	index->put(vertex, k_stage_id);
	std::size_t builds = 0;
	driver::null::driver drv;
	drv.judge_shaders_with([&builds](const std::vector<driver::shader_header>&) -> std::optional<driver::shader_build_log> {
		++builds;
		return std::nullopt;
	});
	ShaderManager shaders(&drv, *rs);
	shader_config::constant_data cdata;
	cdata.program = shader_config::shader_program_data::build().set_vertex_shader(vertex).set_fragment_shader(fragment);
	const auto variant = shaders.variants().intern(cdata);
	BOOST_REQUIRE(shaders.program(variant) != nullptr);
	BOOST_REQUIRE(builds == 1u);

	BOOST_REQUIRE(rs->rename(vertex, renamed));
	pump();
	index->move(vertex, renamed);
	const std::vector<std::pair<res::tag, res::tag>> moved{ { vertex, renamed } };
	rs->apply_asset_batch(rs->delivering_batch(), moved);
	pump();
	shaders.program(variant);
	const std::size_t before = builds;

	rs->signal_changed(renamed);
	pump();
	shaders.program(variant);
	BOOST_TEST(builds == before + 1u);
}

BOOST_AUTO_TEST_SUITE_END()
