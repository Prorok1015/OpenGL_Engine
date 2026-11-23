#include <boost/test/unit_test.hpp>
#include "edt_shader_problems.h"
#include "edt_service_commands.h"
#include <snk/editor/edt_problem_list.h>
#include <snk/render/rnd_shader_problem.h>
#include <snk/command/cmd_registry.h>
#include <boost/json/parse.hpp>
#include <string>
#include <vector>

// #1201: the shader programs that did not build as the Problems source "shader", and problems.list over it.

namespace json = boost::json;

namespace {

rnd::shader_problem compile_error(std::string file, int line, int column, std::string message)
{
	rnd::shader_problem out;
	out.file = res::tag{ file };
	out.line = line;
	out.column = column;
	out.code = "shader/compile";
	out.message = std::move(message);
	out.program = "scene.vert + scene.frag { OPAQUE }";
	return out;
}

}

BOOST_AUTO_TEST_SUITE(EdtShaderProblemsTests)

BOOST_AUTO_TEST_CASE(ARowIsAnErrorAtTheFileAndLineWithTheProgramAsItsNote)
{
	const auto rows = edt::shader_problem_publisher::to_problems({ compile_error("res://shaders/lib.glsl", 12, 5, "error: `x' undeclared") });
	BOOST_REQUIRE(rows.size() == 1u);
	BOOST_TEST((rows[0].severity == edt::problem_severity::error));
	BOOST_TEST(rows[0].file == "res://shaders/lib.glsl");
	BOOST_TEST(rows[0].line == 12);
	BOOST_TEST(rows[0].column == 5);
	BOOST_TEST(rows[0].code == "shader/compile");
	BOOST_TEST(rows[0].message == "error: `x' undeclared");
	BOOST_TEST((rows[0].notes == std::vector<std::string>{ "scene.vert + scene.frag { OPAQUE }" }));
}

BOOST_AUTO_TEST_CASE(TheSourceFollowsTheGenerationAndIsClearedByAFix)
{
	edt::problem_list problems;
	edt::shader_problem_publisher publisher;
	std::vector<rnd::shader_problem> current;
	int asked = 0;
	const auto snapshot = [&] {
		++asked;
		return current;
	};

	publisher.follow(problems, 0, snapshot);
	BOOST_TEST(asked == 0);
	BOOST_TEST(problems.sources().empty());

	current = { compile_error("res://shaders/scene.frag", 7, 0, "error C0000: syntax error") };
	publisher.follow(problems, 1, snapshot);
	BOOST_TEST(asked == 1);
	BOOST_REQUIRE(problems.size() == 1u);
	BOOST_TEST(problems.list()[0].source == "shader");
	BOOST_TEST(problems.list()[0].item.line == 7);

	publisher.follow(problems, 1, snapshot);
	BOOST_TEST(asked == 1);	// the same generation: nothing asked, nothing set

	current.clear();
	publisher.follow(problems, 2, snapshot);
	BOOST_TEST(problems.size() == 0u);
	BOOST_TEST(problems.sources().empty());
}

BOOST_AUTO_TEST_CASE(ProblemsListGivesTheShaderRowsWithTheirLines)
{
	edt::problem_list problems;
	edt::shader_problem_publisher publisher;
	publisher.follow(problems, 1, [] { return std::vector<rnd::shader_problem>{ compile_error("res://shaders/lib.glsl", 12, 5, "error: `x' undeclared") }; });
	problems.set("desc", { edt::problem{} });

	cmd::command_registry commands;
	const std::vector<edt::registration> registrations = edt::register_problem_commands(commands, problems);
	const cmd::result listed = commands.invoke("problems.list", json::parse(R"({"source":"shader"})"), cmd::origin::test());
	BOOST_REQUIRE_MESSAGE(listed.ok, listed.error);
	const json::array& rows = listed.value.as_object().at("problems").as_array();
	BOOST_REQUIRE(rows.size() == 1u);
	const json::object& row = rows[0].as_object();
	BOOST_TEST(std::string(row.at("source").as_string()) == "shader");
	BOOST_TEST(std::string(row.at("severity").as_string()) == "error");
	BOOST_TEST(std::string(row.at("file").as_string()) == "res://shaders/lib.glsl");
	BOOST_TEST(row.at("line").as_int64() == 12);
	BOOST_TEST(row.at("column").as_int64() == 5);
	BOOST_TEST(std::string(row.at("code").as_string()) == "shader/compile");
}

BOOST_AUTO_TEST_SUITE_END()
