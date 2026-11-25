#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_shader_check.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

// #1386: which shader programs a file belongs to, and what the Check reports about them.

namespace fs = std::filesystem;

namespace {

rnd::shader_problem problem_in(const res::tag& file, int line)
{
	rnd::shader_problem out;
	out.file = file;
	out.line = line;
	out.code = "shader/compile";
	out.message = "error";
	return out;
}

void touch(const fs::path& file)
{
	fs::create_directories(file.parent_path());
	std::ofstream(file, std::ios::binary) << "{}";
}

struct check_fixture
{
	std::function<rnd::shader_check_result(const res::tag&)> checker()
	{
		return [this](const res::tag& desc) {
			asked.push_back(std::string(desc.view()));
			const auto found = programs.find(std::string(desc.view()));
			return found == programs.end() ? rnd::shader_check_result{} : found->second;
		};
	}

	std::map<std::string, rnd::shader_check_result> programs;
	std::vector<std::string> asked;
	const res::tag lib = res::tag::make("shaders/lib.glsl");
	const res::tag vert = res::tag::make("shaders/a.vert");
	const res::tag frag = res::tag::make("shaders/a.frag");
};

}

BOOST_FIXTURE_TEST_SUITE(EdtShaderCheck, check_fixture)

BOOST_AUTO_TEST_CASE(ADescIsFoundInEveryLayer_SortedAndOnce)
{
	test_util::temp_dir dir{ "edt_shader_check" };
	touch(dir.path() / "lower" / "shaders" / "b.shader.desc");
	touch(dir.path() / "top" / "shaders" / "a.shader.desc");
	touch(dir.path() / "top" / "shaders" / "b.shader.desc");
	touch(dir.path() / "top" / "shaders" / "a.vert");
	const std::vector<res::tag> found = edt::shader_descs_in({ dir.path() / "lower", dir.path() / "top" });
	BOOST_REQUIRE(found.size() == 2u);
	BOOST_TEST(found[0].view() == "res://shaders/a.shader.desc");
	BOOST_TEST(found[1].view() == "res://shaders/b.shader.desc");
}

BOOST_AUTO_TEST_CASE(ACheckOfADesc_BuildsThatDescOnly_AndNamesTheDefaultVariant)
{
	const res::tag desc = res::tag::make("shaders/a.shader.desc");
	rnd::shader_check_result result;
	result.program = "a.vert + a.frag";
	result.files = { vert, frag };
	result.problems = { problem_in(frag, 3) };
	programs[std::string(desc.view())] = result;
	const edt::shader_check_report report = edt::check_shader_source(desc, { res::tag::make("shaders/other.shader.desc") }, checker());
	BOOST_TEST(asked.size() == 1u);
	BOOST_REQUIRE(report.problems.size() == 1u);
	BOOST_TEST(report.problems[0].program == "default variant: a.vert + a.frag");
	BOOST_TEST(report.summary.find("1 problem") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AnInclude_ChecksTheProgramsThatIncludeIt_AndOnlyThose)
{
	const res::tag uses = res::tag::make("shaders/uses.shader.desc");
	const res::tag other = res::tag::make("shaders/other.shader.desc");
	programs[std::string(uses.view())] = { { problem_in(lib, 2) }, { vert, lib }, "a.vert" };
	programs[std::string(other.view())] = { { problem_in(frag, 9) }, { frag }, "b.frag" };
	const edt::shader_check_report report = edt::check_shader_source(lib, { other, uses }, checker());
	BOOST_TEST(asked.size() == 2u);
	BOOST_TEST(report.built == 2u);
	BOOST_TEST(report.belonging == 1u);
	BOOST_REQUIRE(report.problems.size() == 1u);
	BOOST_TEST(report.problems[0].line == 2);
}

BOOST_AUTO_TEST_CASE(AFileNoProgramIsMadeOf_SaysSo_AndAGoodProgramHasNoProblems)
{
	const res::tag desc = res::tag::make("shaders/a.shader.desc");
	programs[std::string(desc.view())] = { {}, { vert }, "a.vert" };
	const edt::shader_check_report none = edt::check_shader_source(lib, { desc }, checker());
	BOOST_TEST(none.belonging == 0u);
	BOOST_TEST(none.problems.empty());
	BOOST_TEST(none.summary.find("nothing to check") != std::string::npos);
	const edt::shader_check_report good = edt::check_shader_source(vert, { desc }, checker());
	BOOST_TEST(good.belonging == 1u);
	BOOST_TEST(good.summary.find("no problems") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(MoreDescsThanTheLimit_AreNotAllBuilt)
{
	std::vector<res::tag> many;
	for (std::size_t index = 0; index < edt::k_shader_check_max_programs + 5; ++index) {
		many.push_back(res::tag::make("shaders/p" + std::to_string(index) + ".shader.desc"));
	}
	const edt::shader_check_report report = edt::check_shader_source(lib, many, checker());
	BOOST_TEST(report.truncated);
	BOOST_TEST(asked.size() == edt::k_shader_check_max_programs);
}

BOOST_AUTO_TEST_SUITE_END()
