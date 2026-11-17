#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_protocol.h"
#include "edt_recent_projects.h"
#include "edt_settings.h"
#include <filesystem>
#include <optional>

// File > Recent Projects (EPIC-42 #158) is kept per user in edt://settings/recent_projects.json.

namespace {

namespace fs = std::filesystem;

struct recent_projects_fixture : test_util::ram_res_env
{
	test_util::temp_dir dir{ "edt_recent_projects" };
	fs::path data = dir.path() / "data";
	fs::path user = dir.path() / "user";

	recent_projects_fixture()
	{
		BOOST_REQUIRE(fs::create_directories(data));
		BOOST_REQUIRE(edt::register_data_protocol(*rs, data, user));
	}

	// Joins the vfs io thread before the scratch directory goes away.
	~recent_projects_fixture() { rs.reset(); }

	fs::path manifest(const char* project)
	{
		dir.write(fs::path{ "projects" } / project / "project.snkproj", "{}");
		return dir.path() / "projects" / project / "project.snkproj";
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtRecentProjects, recent_projects_fixture)

BOOST_AUTO_TEST_CASE(FirstRun_TheListIsEmpty)
{
	BOOST_TEST(edt::read_recent_projects(*rs).empty());
}

BOOST_AUTO_TEST_CASE(Remember_IsReadBackMostRecentFirst)
{
	const fs::path a = manifest("a");
	const fs::path b = manifest("b");
	edt::remember_recent_project(*rs, a);
	edt::remember_recent_project(*rs, b);

	const auto list = edt::read_recent_projects(*rs);
	BOOST_REQUIRE(list.size() == 2u);
	BOOST_TEST(fs::equivalent(list[0], b));
	BOOST_TEST(fs::equivalent(list[1], a));
	BOOST_TEST(fs::exists(user / "settings" / "recent_projects.json"));
}

BOOST_AUTO_TEST_CASE(ABrokenFile_IsAnEmptyList)
{
	BOOST_REQUIRE(edt::write_setting(*rs, "recent_projects.json", "{ not json"));
	BOOST_TEST(edt::read_recent_projects(*rs).empty());
}

BOOST_AUTO_TEST_CASE(Besides_TheOpenProjectIsRememberedButNotListed)
{
	const fs::path a = manifest("a");
	const fs::path b = manifest("b");
	edt::remember_recent_project(*rs, a);

	const auto shown = edt::recent_projects_besides(*rs, b);
	BOOST_REQUIRE(shown.size() == 1u);
	BOOST_TEST(fs::equivalent(shown[0], a));

	const auto stored = edt::read_recent_projects(*rs);
	BOOST_REQUIRE(stored.size() == 2u);
	BOOST_TEST(fs::equivalent(stored[0], b));
}

BOOST_AUTO_TEST_CASE(Besides_WithoutAProject_IsTheStoredList)
{
	const fs::path a = manifest("a");
	edt::remember_recent_project(*rs, a);

	const auto shown = edt::recent_projects_besides(*rs, std::nullopt);
	BOOST_REQUIRE(shown.size() == 1u);
	BOOST_TEST(fs::equivalent(shown[0], a));
}

BOOST_AUTO_TEST_SUITE_END()
