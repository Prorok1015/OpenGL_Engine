#include <boost/test/unit_test.hpp>
#include <snk/project/prj_recent.h>
#include "test_temp_dir.hpp"
#include <filesystem>
#include <string>
#include <string_view>

// File > Recent Projects (EPIC-42 #158): the list the editor keeps in edt://settings/recent_projects.json.

namespace {
	namespace fs = std::filesystem;

	struct recent_fixture
	{
		test_util::temp_dir dir{ "prj_recent" };

		fs::path project(std::string_view name) const
		{
			return dir.path() / name / "project.snkproj";
		}
	};
}

BOOST_FIXTURE_TEST_SUITE(PrjRecentTests, recent_fixture)

BOOST_AUTO_TEST_CASE(Remember_PutsTheProjectFirst)
{
	auto list = prj::remember_project({}, project("a"));
	list = prj::remember_project(list, project("b"));

	BOOST_REQUIRE(list.size() == 2u);
	BOOST_TEST(list[0] == project("b"));
	BOOST_TEST(list[1] == project("a"));
}

BOOST_AUTO_TEST_CASE(Remember_AgainMovesItUpWithoutADuplicate)
{
	auto list = prj::remember_project({}, project("a"));
	list = prj::remember_project(list, project("b"));
	// Another spelling of the same file counts as the same project.
	list = prj::remember_project(list, dir.path() / "b" / ".." / "a" / "project.snkproj");

	BOOST_REQUIRE(list.size() == 2u);
	BOOST_TEST(list[1] == project("b"));
}

BOOST_AUTO_TEST_CASE(Remember_KeepsTheFiveMostRecent)
{
	prj::recent_projects list;
	for (const char* name : { "a", "b", "c", "d", "e", "f" }) {
		list = prj::remember_project(list, project(name));
	}

	BOOST_REQUIRE(list.size() == prj::k_max_recent_projects);
	BOOST_TEST(list.front() == project("f"));
	BOOST_TEST(list.back() == project("b"));
}

BOOST_AUTO_TEST_CASE(Serialize_ThenParse_GivesTheSameList)
{
	prj::recent_projects list;
	for (const char* name : { "a", "b", "c" }) {
		list = prj::remember_project(list, project(name));
	}

	BOOST_TEST((prj::parse_recent_projects(prj::serialize_recent_projects(list)) == list));
}

BOOST_AUTO_TEST_CASE(Parse_AnythingButTheFormatIsAnEmptyList)
{
	for (const std::string_view text : { "", "not json", "[]", "{}", R"({ "format_version": 2, "projects": [] })",
			R"({ "format_version": "1", "projects": [] })", R"({ "format_version": 1, "projects": "a" })" }) {
		BOOST_TEST_INFO("text: " << text);
		BOOST_TEST(prj::parse_recent_projects(text).empty());
	}
}

BOOST_AUTO_TEST_CASE(Parse_DropsRelativePathsDuplicatesAndTheSurplus)
{
	prj::recent_projects many;
	for (const char* name : { "a", "b", "c", "d", "e", "f" }) {
		many.push_back(project(name));
	}
	std::string text = prj::serialize_recent_projects(many);
	// A relative entry and a duplicate of "a", written by hand in front of the others.
	text.replace(text.find('[') + 1, 0, "\"games/x/project.snkproj\", \"" + project("a").generic_string() + "\", 7, ");

	const auto list = prj::parse_recent_projects(text);
	BOOST_REQUIRE(list.size() == prj::k_max_recent_projects);
	BOOST_TEST(list.front() == project("a"));
	BOOST_TEST(list.back() == project("e"));
}

BOOST_AUTO_TEST_SUITE_END()
