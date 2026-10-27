#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_protocol.h"
#include "edt_settings.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

// edt://settings/ (#164): per-user editor settings are written to the user root of edt:// and read back through the resource system.

namespace {

struct settings_fixture : test_util::ram_res_env
{
	test_util::temp_dir dir{ "edt_settings" };
	std::filesystem::path data = dir.path() / "data";
	std::filesystem::path user = dir.path() / "user";

	settings_fixture()
	{
		BOOST_REQUIRE(std::filesystem::create_directories(data));
		BOOST_REQUIRE(edt::register_data_protocol(*rs, data, user));
	}

	// Joins the vfs io thread before the scratch directory goes away.
	~settings_fixture() { rs.reset(); }

	// The vfs resolver writes on its io thread.
	static bool wait_for_text(const std::filesystem::path& file, const std::string& text)
	{
		for (int i = 0; i < 500; ++i) {
			std::ifstream in(file, std::ios::binary);
			const std::string content{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
			if (in.is_open() && content == text) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtSettings, settings_fixture)

BOOST_AUTO_TEST_CASE(SettingsTag_PointsIntoTheSettingsFolderOfEdt)
{
	BOOST_TEST(edt::settings_tag("layout.ini").string() == "edt://settings/layout.ini");
}

BOOST_AUTO_TEST_CASE(ReadSetting_MissingFile_IsNullopt)
{
	BOOST_TEST(!edt::read_setting(*rs, "never_written.json").has_value());
}

BOOST_AUTO_TEST_CASE(WriteSetting_ThenRead_ReturnsTheText)
{
	BOOST_REQUIRE(edt::write_setting(*rs, "recent_files.json", R"({"files":["a.desc"]})"));

	const auto text = edt::read_setting(*rs, "recent_files.json");
	BOOST_REQUIRE(text.has_value());
	BOOST_TEST(*text == R"({"files":["a.desc"]})");
}

BOOST_AUTO_TEST_CASE(WriteSetting_Twice_ReadSeesTheLatestText)
{
	BOOST_REQUIRE(edt::write_setting(*rs, "user_prefs.cfg", "theme: dark"));
	BOOST_REQUIRE(edt::read_setting(*rs, "user_prefs.cfg").has_value());

	BOOST_REQUIRE(edt::write_setting(*rs, "user_prefs.cfg", "theme: light"));
	BOOST_TEST(edt::read_setting(*rs, "user_prefs.cfg").value_or("") == "theme: light");
}

BOOST_AUTO_TEST_CASE(WriteSetting_LandsInTheUserRootNeverInTheShippedData)
{
	BOOST_REQUIRE(edt::write_setting(*rs, "layout.ini", "[Window][Main]"));

	BOOST_TEST(wait_for_text(user / "settings" / "layout.ini", "[Window][Main]"));
	BOOST_TEST(!std::filesystem::exists(data / "settings"));
}

BOOST_AUTO_TEST_CASE(WriteSetting_NameEscapingTheSettingsFolder_IsNotReadBack)
{
	edt::write_setting(*rs, "../../escaped.txt", "x");

	BOOST_TEST(!edt::read_setting(*rs, "../../escaped.txt").has_value());
	BOOST_TEST(!std::filesystem::exists(dir.path() / "escaped.txt"));
}

BOOST_AUTO_TEST_SUITE_END()
