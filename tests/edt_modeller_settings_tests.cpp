#include <boost/test/unit_test.hpp>
#include "modeller/edt_modeller_settings.h"
#include <limits>

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

BOOST_AUTO_TEST_CASE(Clamped_ANonFiniteStep_BecomesTheSmallest)
{
	edt::modeller_settings settings;
	settings.move_step = std::numeric_limits<float>::quiet_NaN();
	BOOST_TEST(edt::clamped(settings).move_step == edt::k_modeller_min_step);
}

BOOST_AUTO_TEST_SUITE_END()
