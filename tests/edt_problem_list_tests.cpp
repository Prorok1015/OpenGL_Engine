#include <boost/test/unit_test.hpp>
#include "edt_problems_panel.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/editor/edt_service_commands.h>
#include <snk/command/cmd_json_schema.h>
#include <boost/json/parse.hpp>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// problem_list (the editor's problems by source), problems.list over it, and what the Problems panel shows (#434).

namespace json = boost::json;

namespace {

edt::problem make(edt::problem_severity severity, std::string message, std::string file = {}, int line = 0)
{
	edt::problem out;
	out.severity = severity;
	out.file = std::move(file);
	out.line = line;
	out.message = std::move(message);
	return out;
}

std::vector<std::string> messages_of(const edt::problem_list& problems)
{
	std::vector<std::string> out;
	for (const auto& entry : problems.list()) out.push_back(entry.source + ":" + entry.item.message);
	return out;
}

struct problem_list_fixture
{
	problem_list_fixture()
	{
		subscription = events.problems_changed.subscribe([this](const std::string& source) { changed.push_back(source); });
	}

	edt::editor_events events;
	edt::problem_list problems{ &events };
	std::vector<std::string> changed;
	edt::connection subscription;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtProblemListTests, problem_list_fixture)

BOOST_AUTO_TEST_CASE(Set_ReplacesTheProblemsOfOneSourceOnly)
{
	problems.set("build", { make(edt::problem_severity::error, "a"), make(edt::problem_severity::warning, "b") });
	problems.set("desc", { make(edt::problem_severity::error, "c") });
	BOOST_TEST((messages_of(problems) == std::vector<std::string>{ "build:a", "build:b", "desc:c" }));

	problems.set("build", { make(edt::problem_severity::note, "d") });
	BOOST_TEST((messages_of(problems) == std::vector<std::string>{ "build:d", "desc:c" }));
	BOOST_TEST((problems.sources() == std::vector<std::string>{ "build", "desc" }));
	BOOST_TEST(problems.size() == 2u);
}

BOOST_AUTO_TEST_CASE(Clear_DropsTheSource_AndASetAfterItGoesLast)
{
	problems.set("build", { make(edt::problem_severity::error, "a") });
	problems.set("desc", { make(edt::problem_severity::error, "b") });
	problems.clear("build");
	BOOST_TEST((messages_of(problems) == std::vector<std::string>{ "desc:b" }));
	problems.set("build", { make(edt::problem_severity::error, "c") });
	BOOST_TEST((problems.sources() == std::vector<std::string>{ "desc", "build" }));
}

BOOST_AUTO_TEST_CASE(Count_IsPerSeverityOverAllSources)
{
	problems.set("build", { make(edt::problem_severity::error, "a"), make(edt::problem_severity::warning, "b"),
		make(edt::problem_severity::error, "c") });
	problems.set("desc", { make(edt::problem_severity::error, "d"), make(edt::problem_severity::note, "e") });
	BOOST_TEST(problems.count(edt::problem_severity::error) == 3u);
	BOOST_TEST(problems.count(edt::problem_severity::warning) == 1u);
	BOOST_TEST(problems.count(edt::problem_severity::note) == 1u);
	problems.clear("build");
	BOOST_TEST(problems.count(edt::problem_severity::error) == 1u);
}

BOOST_AUTO_TEST_CASE(EveryChange_IsAnnounced_AndBumpsTheGeneration)
{
	const auto start = problems.generation();
	problems.set("build", {});
	problems.set("build", { make(edt::problem_severity::error, "a") });
	problems.clear("build");
	BOOST_TEST((changed == std::vector<std::string>{ "build", "build", "build" }));
	BOOST_TEST(problems.generation() == start + 3);

	problems.clear("unknown");
	BOOST_TEST(changed.size() == 3u);
	BOOST_TEST(problems.generation() == start + 3);
}

BOOST_AUTO_TEST_CASE(ASlotMaySetAnotherSource)
{
	edt::connection nested = events.problems_changed.subscribe([this](const std::string& source) {
		if (source == "build") problems.set("desc", { make(edt::problem_severity::warning, "from slot") });
	});
	problems.set("build", { make(edt::problem_severity::error, "a") });
	BOOST_TEST((changed == std::vector<std::string>{ "build", "desc" }));
	BOOST_TEST((messages_of(problems) == std::vector<std::string>{ "build:a", "desc:from slot" }));
}

BOOST_AUTO_TEST_CASE(SeverityNames_RoundTrip)
{
	for (const auto severity : { edt::problem_severity::error, edt::problem_severity::warning, edt::problem_severity::note }) {
		const auto back = edt::problem_severity_from(edt::problem_severity_name(severity));
		BOOST_TEST_REQUIRE(back.has_value());
		BOOST_TEST((*back == severity));
	}
	BOOST_TEST(!edt::problem_severity_from("fatal").has_value());
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

struct problem_command_fixture : problem_list_fixture
{
	problem_command_fixture()
		: registrations(edt::register_problem_commands(commands, problems))
	{
		edt::problem with_details = make(edt::problem_severity::error, "undeclared", "a.cpp", 12);
		with_details.column = 5;
		with_details.code = "C2065";
		with_details.notes = { "a.h(3): note: see declaration" };
		problems.set("build", { with_details, make(edt::problem_severity::warning, "unused", "b.cpp", 3) });
		problems.set("desc", { make(edt::problem_severity::error, "bad field") });
	}

	cmd::result call(const std::string& args = "{}")
	{
		return commands.invoke("problems.list", json::parse(args), cmd::origin::test());
	}

	std::vector<std::string> messages(const cmd::result& result)
	{
		std::vector<std::string> out;
		for (const auto& entry : result.value.as_object().at("problems").as_array()) {
			const auto& object = entry.as_object();
			out.push_back(std::string(object.at("source").as_string()) + ":" + std::string(object.at("message").as_string()));
		}
		return out;
	}

	cmd::command_registry commands;
	std::vector<edt::registration> registrations;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtProblemCommandsTests, problem_command_fixture)

BOOST_AUTO_TEST_CASE(Registered_ProblemsListOnly_AndItChangesNothing)
{
	BOOST_TEST(registrations.size() == 1u);
	const cmd::command_desc* desc = commands.find("problems.list");
	BOOST_TEST_REQUIRE(desc != nullptr);
	BOOST_TEST(!desc->mutates);
	BOOST_TEST((desc->risk == cmd::danger::none));
}

BOOST_AUTO_TEST_CASE(ProblemsList_ListsEverything_InOrder_WithEveryField)
{
	const cmd::result listed = call();
	BOOST_REQUIRE_MESSAGE(listed.ok, listed.error);
	BOOST_TEST(!cmd::json_schema::validate(commands.find("problems.list")->result_schema, listed.value).has_value());
	BOOST_TEST((messages(listed) == std::vector<std::string>{ "build:undeclared", "build:unused", "desc:bad field" }));

	const json::object& first = listed.value.as_object().at("problems").as_array()[0].as_object();
	BOOST_TEST(std::string(first.at("severity").as_string()) == "error");
	BOOST_TEST(std::string(first.at("file").as_string()) == "a.cpp");
	BOOST_TEST(first.at("line").as_int64() == 12);
	BOOST_TEST(first.at("column").as_int64() == 5);
	BOOST_TEST(std::string(first.at("code").as_string()) == "C2065");
	BOOST_TEST_REQUIRE(first.at("notes").as_array().size() == 1u);
	BOOST_TEST(std::string(first.at("notes").as_array()[0].as_string()) == "a.h(3): note: see declaration");

	const json::object& last = listed.value.as_object().at("problems").as_array()[2].as_object();
	BOOST_TEST(std::string(last.at("file").as_string()) == "");
	BOOST_TEST(last.at("line").as_int64() == 0);
	BOOST_TEST(last.at("notes").as_array().empty());
}

BOOST_AUTO_TEST_CASE(ProblemsList_FiltersBySourceAndSeverity)
{
	BOOST_TEST((messages(call(R"({"source":"desc"})")) == std::vector<std::string>{ "desc:bad field" }));
	BOOST_TEST((messages(call(R"({"severity":"error"})")) == std::vector<std::string>{ "build:undeclared", "desc:bad field" }));
	BOOST_TEST((messages(call(R"({"source":"build","severity":"warning"})")) == std::vector<std::string>{ "build:unused" }));
	BOOST_TEST(messages(call(R"({"source":"nothing"})")).empty());
}

BOOST_AUTO_TEST_CASE(ProblemsList_RejectsBadArguments)
{
	BOOST_TEST(call(R"({"severity":"fatal"})").code == "invalid_args");
	BOOST_TEST(call(R"({"source":1})").code == "invalid_args");
	BOOST_TEST(call(R"({"extra":1})").code == "invalid_args");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtProblemsPanelTextTests)

BOOST_AUTO_TEST_CASE(DisplayFile_IsRelativeUnderTheProjectRoot_ElseAsPrinted)
{
	const std::filesystem::path root = std::filesystem::current_path() / "project";
	const std::string inside = (root / "src" / "a.cpp").string();
	BOOST_TEST(edt::problem_display_file(inside, root) == "src/a.cpp");

	const std::string outside = (std::filesystem::current_path() / "other" / "b.cpp").string();
	BOOST_TEST(edt::problem_display_file(outside, root) == outside);
	BOOST_TEST(edt::problem_display_file("src/a.cpp", root) == "src/a.cpp");
	BOOST_TEST(edt::problem_display_file(inside, {}) == inside);
	BOOST_TEST(edt::problem_display_file("", root) == "");
}

// cl prints paths in the console code page (here "Ivan" in Cyrillic, CP1251), which is not UTF-8.
BOOST_AUTO_TEST_CASE(DisplayFile_OfInvalidUtf8_IsAsPrinted_AndDoesNotThrow)
{
	const std::filesystem::path root = std::filesystem::current_path();
	for (const std::string& file : { std::string("C:\\Users\\\xC8\xE2\xE0\xED\\a.cpp"), std::string("/home/\xC8\xE2/a.cpp"),
		(root / "x").string() + "\xFF/a.cpp" }) {
		std::string shown;
		BOOST_CHECK_NO_THROW(shown = edt::problem_display_file(file, root));
		BOOST_TEST(shown == file);
	}
}

BOOST_AUTO_TEST_CASE(CopyText_IsFileLineSeverityCodeMessage)
{
	edt::problem item = make(edt::problem_severity::error, "'x': undeclared identifier", "a.cpp", 12);
	item.code = "C2065";
	BOOST_TEST(edt::problem_copy_text(item) == "a.cpp(12): error C2065: 'x': undeclared identifier");

	BOOST_TEST(edt::problem_copy_text(make(edt::problem_severity::warning, "unused", "b.cpp")) == "b.cpp: warning: unused");
	BOOST_TEST(edt::problem_copy_text(make(edt::problem_severity::error, "ld returned 1 exit status")) == "error: ld returned 1 exit status");

	edt::problem detailed = make(edt::problem_severity::error, "boom", "/p/a.cpp", 3);
	detailed.column = 5;
	detailed.notes = { "/p/a.h:1:1: note: here" };
	BOOST_TEST(edt::problem_copy_text(detailed) == "/p/a.cpp(3,5): error: boom\n/p/a.h:1:1: note: here");
}

BOOST_AUTO_TEST_SUITE_END()
