#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_data_protocol.h"
#include "edt_settings.h"
#include "modeller/edt_modeller_settings.h"
#include <filesystem>
#include <limits>

namespace {

struct modeller_settings_fixture : test_util::ram_res_env
{
	test_util::temp_dir dir{ "edt_modeller_settings" };
	std::filesystem::path data = dir.path() / "data";
	std::filesystem::path user = dir.path() / "user";

	modeller_settings_fixture()
	{
		BOOST_REQUIRE(std::filesystem::create_directories(data));
		BOOST_REQUIRE(edt::register_data_protocol(*rs, data, user));
	}

	~modeller_settings_fixture() { rs.reset(); }
};

}

BOOST_FIXTURE_TEST_SUITE(EdtModellerSettingsFiles, modeller_settings_fixture)

BOOST_AUTO_TEST_CASE(Load_ACorruptFile_GivesTheDefaults_AndASaveThenLoadRoundTrips)
{
	BOOST_REQUIRE(edt::write_setting(*rs, edt::k_modeller_settings_name, "{ not json"));
	BOOST_TEST((edt::load_modeller_settings(*rs) == edt::modeller_settings{}));

	edt::modeller_settings settings;
	settings.move_step = 0.25f;
	settings.rotate_step_degrees = 30.f;
	BOOST_REQUIRE(edt::save_modeller_settings(*rs, settings));
	BOOST_TEST((edt::load_modeller_settings(*rs) == settings));
}

BOOST_AUTO_TEST_CASE(Load_AMissingFile_IsTheDefaults)
{
	BOOST_TEST((edt::load_modeller_settings(*rs) == edt::modeller_settings{}));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtModellerSettings)

BOOST_AUTO_TEST_CASE(Defaults_AreTheStepsTheOwnerChose)
{
	const edt::modeller_settings defaults;
	BOOST_TEST(defaults.move_step == 0.1f);
	BOOST_TEST(defaults.rotate_step_degrees == 15.f);
	BOOST_TEST(defaults.scale_step == 0.1f);
}

BOOST_AUTO_TEST_CASE(Parse_AnEmptyObject_IsTheDefaults)
{
	const auto parsed = edt::parse_modeller_settings("{}");
	BOOST_TEST(parsed.error.empty());
	BOOST_TEST((parsed.settings == edt::modeller_settings{}));
}

BOOST_AUTO_TEST_CASE(Parse_TakesEveryKey_AndClampsTheRange)
{
	const auto parsed = edt::parse_modeller_settings(R"({"move_step": 0.5, "rotate_step_degrees": 500, "scale_step": 0})");
	BOOST_TEST(parsed.error.empty());
	BOOST_TEST(parsed.settings.move_step == 0.5f);
	BOOST_TEST(parsed.settings.rotate_step_degrees == edt::k_modeller_max_rotate_step);
	BOOST_TEST(parsed.settings.scale_step == edt::k_modeller_min_step);
}

BOOST_AUTO_TEST_CASE(Parse_AnUnknownKeyAWrongTypeAndNoObject_AreErrors)
{
	BOOST_TEST(!edt::parse_modeller_settings(R"({"snap": 1})").error.empty());
	BOOST_TEST(!edt::parse_modeller_settings(R"({"move_step": "big"})").error.empty());
	BOOST_TEST(!edt::parse_modeller_settings("[1]").error.empty());
	BOOST_TEST(!edt::parse_modeller_settings("{").error.empty());
}

BOOST_AUTO_TEST_CASE(Serialize_ThenParse_GivesTheSameSettings)
{
	edt::modeller_settings settings;
	settings.move_step = 0.25f;
	settings.rotate_step_degrees = 45.f;
	settings.scale_step = 2.f;
	const auto parsed = edt::parse_modeller_settings(edt::serialize_modeller_settings(settings));
	BOOST_TEST(parsed.error.empty());
	BOOST_TEST((parsed.settings == settings));
}

BOOST_AUTO_TEST_CASE(Clamped_ANonFiniteStep_BecomesTheDefaultStep)
{
	edt::modeller_settings settings;
	settings.move_step = std::numeric_limits<float>::quiet_NaN();
	settings.rotate_step_degrees = std::numeric_limits<float>::infinity();
	BOOST_TEST(edt::clamped(settings).move_step == edt::modeller_settings{}.move_step);
	BOOST_TEST(edt::clamped(settings).rotate_step_degrees == edt::modeller_settings{}.rotate_step_degrees);
}

BOOST_AUTO_TEST_SUITE_END()
