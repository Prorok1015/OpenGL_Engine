#include <boost/test/unit_test.hpp>
#include "edt_agent_server.h"
#include <filesystem>
#include <optional>

// Where the editor writes .snk/mcp.json (EPIC-42 #155).
BOOST_AUTO_TEST_SUITE(EdtAgentProjectDirTests)

BOOST_AUTO_TEST_CASE(ConfiguredDirWinsOverTheProject)
{
	BOOST_TEST(edt::agent_project_dir("C:/agents", std::filesystem::path{ "/games/sandbox" }, "/user") == std::filesystem::path{ "C:/agents" });
}

BOOST_AUTO_TEST_CASE(OpenProjectRootWhenNothingIsConfigured)
{
	BOOST_TEST(edt::agent_project_dir({}, std::filesystem::path{ "/games/sandbox" }, "/user") == std::filesystem::path{ "/games/sandbox" });
}

BOOST_AUTO_TEST_CASE(UserFolderWithoutAProject_NeverTheWorkingDirectory)
{
	BOOST_TEST(edt::agent_project_dir({}, std::nullopt, "/user") == std::filesystem::path{ "/user" });
}

BOOST_AUTO_TEST_SUITE_END()
