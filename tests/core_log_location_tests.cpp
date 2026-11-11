#include <boost/test/unit_test.hpp>
#include <snk/config/cfg_variables.h>
#include <snk/core/core_log_location.h>
#include <snk/editor/editor_module.h>
#include "test_temp_dir.hpp"
#include <filesystem>
#include <system_error>

// Where a process writes its logs and its window layout (#713): never the folder it was started from.

namespace fs = std::filesystem;

namespace {

// A link needs a privilege on Windows: the tests that make one skip when it cannot be made.
bool make_dir_link(const fs::path& target, const fs::path& link)
{
	std::error_code ec;
	fs::create_directory_symlink(target, link, ec);
	return !ec;
}

}

BOOST_AUTO_TEST_SUITE(CoreLogLocationTests)

BOOST_AUTO_TEST_CASE(TheUserLogRoot_IsLocalAppDataOnWindows_AndTheXdgStateFolderElsewhere)
{
	BOOST_CHECK(core::user_log_root_for(true, "C:/Users/a/AppData/Local", "", "") == fs::path("C:/Users/a/AppData/Local") / "Snake");
	BOOST_CHECK(core::user_log_root_for(true, "", "/state", "/home/a").empty());

	BOOST_CHECK(core::user_log_root_for(false, "", "/state", "/home/a") == fs::path("/state") / "snake");
	// A relative XDG_STATE_HOME is not a place: the specification says to ignore it.
	BOOST_CHECK(core::user_log_root_for(false, "", "state", "/home/a") == fs::path("/home/a") / ".local" / "state" / "snake");
	BOOST_CHECK(core::user_log_root_for(false, "", "", "/home/a") == fs::path("/home/a") / ".local" / "state" / "snake");
	BOOST_CHECK(core::user_log_root_for(false, "", "", "").empty());
}

BOOST_AUTO_TEST_CASE(ARelativeLogPath_IsTakenFromTheLogRoot_NotFromTheCurrentFolder)
{
	const fs::path root = fs::path("/project/.cache");
	BOOST_CHECK(core::resolve_log_path("logs/engine.log", root) == root / "logs" / "engine.log");
	BOOST_CHECK(core::resolve_log_path("logs/../editor.log", root) == root / "editor.log");
	BOOST_CHECK(core::resolve_log_path("engine.log", root) == root / "engine.log");
}

BOOST_AUTO_TEST_CASE(AnAbsoluteLogPath_StaysAsItIs)
{
	const fs::path absolute = fs::absolute(fs::path("elsewhere") / "x.log");
	BOOST_CHECK(core::resolve_log_path(absolute.string(), fs::path("/project/.cache")) == absolute);
}

BOOST_AUTO_TEST_CASE(ALogPathThatLeavesTheRoot_OrWithoutAValue_IsNone)
{
	const fs::path root = fs::path("/project/.cache");
	BOOST_CHECK(core::resolve_log_path("../outside.log", root).empty());
	BOOST_CHECK(core::resolve_log_path("logs/../../outside.log", root).empty());
	BOOST_CHECK(core::resolve_log_path("", root).empty());
	BOOST_CHECK(core::resolve_log_path("logs/engine.log", fs::path{}).empty());
}

BOOST_AUTO_TEST_CASE(FoldersThatDoNotExistYet_AreNoProblem)
{
	test_util::temp_dir project("log_location_nothing_yet");
	const fs::path file = project.path() / ".cache" / "logs" / "engine.log";
	BOOST_CHECK(core::log_path_problem(project.path(), file).empty());
}

BOOST_AUTO_TEST_CASE(ACacheFolderThatIsALink_IsAProblem_SoNoLogLandsOutsideTheProject)
{
	test_util::temp_dir project("log_location_cache_link");
	test_util::temp_dir outside("log_location_cache_link_target");
	if (!make_dir_link(outside.path(), project.path() / ".cache")) {
		BOOST_TEST_MESSAGE("directory links cannot be made here: skipped");
		return;
	}
	const std::string problem = core::log_path_problem(project.path(), project.path() / ".cache" / "logs" / "engine.log");
	BOOST_CHECK_MESSAGE(problem.find("is a link") != std::string::npos, problem);
}

BOOST_AUTO_TEST_CASE(ALogsFolderThatIsALink_OrNotAFolder_IsAProblem)
{
	test_util::temp_dir project("log_location_logs_link");
	test_util::temp_dir outside("log_location_logs_link_target");
	fs::create_directories(project.path() / ".cache");
	if (make_dir_link(outside.path(), project.path() / ".cache" / "logs")) {
		BOOST_CHECK(!core::log_path_problem(project.path(), project.path() / ".cache" / "logs" / "engine.log").empty());
	}

	test_util::temp_dir blocked("log_location_logs_is_a_file");
	blocked.write(".cache/logs", "not a folder");
	const std::string problem = core::log_path_problem(blocked.path(), blocked.path() / ".cache" / "logs" / "engine.log");
	BOOST_CHECK_MESSAGE(problem.find("not a folder") != std::string::npos, problem);
}

BOOST_AUTO_TEST_CASE(APathOutsideTheTrustedRoot_OrWithoutOne_IsNotJudged)
{
	test_util::temp_dir project("log_location_not_judged");
	BOOST_CHECK(core::log_path_problem(project.path(), fs::absolute("somewhere_else.log")).empty());
	BOOST_CHECK(core::log_path_problem(fs::path{}, project.path() / "engine.log").empty());
}

// editor::apply_default_settings: the window layout goes to the editor's user folder unless the config already named a file.
struct layout_settings_fixture
{
	cfg::variable<fs::path> layout{ "gui/layout_file", "" };
	cfg::variable<fs::path> user_data{ "editor/user_data_path", "" };

	~layout_settings_fixture()
	{
		cfg::set_variable("gui/layout_file", "");
		cfg::set_variable("editor/user_data_path", "");
	}
};

BOOST_FIXTURE_TEST_CASE(TheEditorsWindowLayout_DefaultsToItsUserFolder_NotTheCurrentOne, layout_settings_fixture)
{
	test_util::temp_dir user("log_location_layout_default");
	cfg::set_variable("editor/user_data_path", user.path().string());
	editor::apply_default_settings();

	BOOST_CHECK(layout->is_absolute());
	BOOST_CHECK(layout->lexically_normal() == (user.path() / "settings" / "layout_settings.ini").lexically_normal());
}

BOOST_FIXTURE_TEST_CASE(AFileTheConfigNamedForTheLayout_IsKept, layout_settings_fixture)
{
	test_util::temp_dir user("log_location_layout_given");
	cfg::set_variable("editor/user_data_path", user.path().string());
	const fs::path given = user.path() / "mine.ini";
	cfg::set_variable("gui/layout_file", given.string());
	editor::apply_default_settings();

	BOOST_CHECK(layout->lexically_normal() == given.lexically_normal());
}

BOOST_AUTO_TEST_SUITE_END()
