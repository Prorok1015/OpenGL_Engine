#include <boost/test/unit_test.hpp>
#include "edt_engine_version_prompt.h"
#include <boost/json/parse.hpp>
#include <functional>
#include <string>
#include <string_view>

// The dialog the editor shows for a project held back by its engine version (EPIC-42 #156).
namespace {

struct prompt_fixture
{
	prompt_fixture()
	{
		// Nothing is drawn: answers arrive through dialog_service::answer, as a script or an agent gives them.
		dialogs.set_registrar([](std::string_view, std::function<bool()>) {});
		held.name = "Sandbox";
		held.engine = prj::engine_version{ 0, 1, 0 };
		held.action = prj::version_action::ask_open_or_quit;
	}

	edt::dialog_service::dialog_id ask()
	{
		edt::ask_about_held_project(dialogs, held, [this] { ++quits; });
		BOOST_REQUIRE(dialogs.current().has_value());
		return *dialogs.current();
	}

	// As the editor asks it: with "Open Anyway" (EPIC-42 #158).
	edt::dialog_service::dialog_id ask_with_open_anyway()
	{
		edt::ask_about_held_project(dialogs, held, [this] { ++quits; }, [this] { ++reopened; });
		BOOST_REQUIRE(dialogs.current().has_value());
		return *dialogs.current();
	}

	edt::dialog_service dialogs;
	prj::held_project held;
	int quits = 0;
	int reopened = 0;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtEngineVersionPromptTests, prompt_fixture)

BOOST_AUTO_TEST_CASE(Prompt_NamesBothVersionsAndTheFlag)
{
	ask();
	const auto list = dialogs.list();
	BOOST_REQUIRE(list.size() == 1u);
	BOOST_TEST(list[0].kind == "choice");
	BOOST_TEST(list[0].text.find("0.1.0") != std::string::npos);
	BOOST_TEST(list[0].text.find(prj::to_string(prj::current_engine_version())) != std::string::npos);
	BOOST_TEST(list[0].text.find("--allow-engine-version-mismatch") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Prompt_RefusedProjectDoesNotSuggestTheFlag)
{
	held.action = prj::version_action::refuse;
	ask();
	BOOST_TEST(dialogs.list()[0].text.find("--allow-engine-version-mismatch") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(Prompt_QuitRecordsTheAnswerAndQuits)
{
	BOOST_TEST(dialogs.answer(ask(), boost::json::parse(R"({"index":1})")).ok);
	BOOST_TEST((held.answer == prj::held_project::decision::quit));
	BOOST_TEST(quits == 1);
	BOOST_TEST(prj::exit_code_for(held) == prj::k_exit_engine_version);
}

BOOST_AUTO_TEST_CASE(Prompt_ContinueAndEscapeKeepTheEditorRunning)
{
	BOOST_TEST(dialogs.answer(ask(), boost::json::parse(R"({"index":0})")).ok);
	BOOST_TEST((held.answer == prj::held_project::decision::continue_without_project));
	BOOST_TEST(quits == 0);
	BOOST_TEST(prj::exit_code_for(held) == 0);

	held.answer = prj::held_project::decision::none;
	BOOST_TEST(dialogs.answer(ask(), boost::json::parse(R"({"cancel":true})")).ok);
	BOOST_TEST((held.answer == prj::held_project::decision::continue_without_project));
	BOOST_TEST(quits == 0);
}

BOOST_AUTO_TEST_CASE(Prompt_UnansweredMeansAnErrorExit)
{
	ask();
	BOOST_TEST((held.answer == prj::held_project::decision::none));
	BOOST_TEST(prj::exit_code_for(held) == prj::k_exit_engine_version);
}

BOOST_AUTO_TEST_CASE(OpenAnyway_IsTheFirstButtonForAnOlderProject)
{
	BOOST_TEST(dialogs.answer(ask_with_open_anyway(), boost::json::parse(R"({"index":0})")).ok);
	BOOST_TEST(reopened == 1);
	BOOST_TEST(quits == 0);
	// This editor goes on (and exits normally) while the other one opens the project.
	BOOST_TEST(prj::exit_code_for(held) == 0);
}

BOOST_AUTO_TEST_CASE(OpenAnyway_KeepsContinueAndQuitAfterIt)
{
	BOOST_TEST(dialogs.answer(ask_with_open_anyway(), boost::json::parse(R"({"index":2})")).ok);
	BOOST_TEST(quits == 1);
	BOOST_TEST(reopened == 0);

	held.answer = prj::held_project::decision::none;
	BOOST_TEST(dialogs.answer(ask_with_open_anyway(), boost::json::parse(R"({"index":1})")).ok);
	BOOST_TEST((held.answer == prj::held_project::decision::continue_without_project));
	BOOST_TEST(reopened == 0);
}

BOOST_AUTO_TEST_CASE(OpenAnyway_IsNotOfferedForANewerProject)
{
	held.action = prj::version_action::refuse;
	BOOST_TEST(dialogs.answer(ask_with_open_anyway(), boost::json::parse(R"({"index":1})")).ok);
	BOOST_TEST(quits == 1);
	BOOST_TEST(reopened == 0);
}

BOOST_AUTO_TEST_SUITE_END()
