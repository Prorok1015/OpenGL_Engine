#include <boost/test/unit_test.hpp>
#include "edt_build_settings.h"
#include "edt_live_consent.h"
#include <snk/common/logger/engine_log_sinks.h>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace {

	constexpr std::string_view k_project = "C:/work/game";
	constexpr std::string_view k_other = "C:/work/other";

	edt::build_settings asking_for_live()
	{
		edt::build_settings settings;
		settings.live_enabled = true;
		return settings;
	}

}

BOOST_AUTO_TEST_SUITE(EdtLiveConsentTests)

// #908: a project whose .snk/build.json asks for live mode opens with it off, and says why.
BOOST_AUTO_TEST_CASE(AProjectAskingForLiveModeOpensWithItOff)
{
	std::size_t warnings = 0;
	const engine::log_sink_handle sink = engine::add_log_sink([&](std::string_view category, engine::log_level level,
		std::string_view message) {
		if (category == "editor/build" && level == engine::log_level::warn
			&& message.find("live_enabled in .snk/build.json is ignored") != std::string_view::npos) {
			++warnings;
		}
	});
	BOOST_TEST(!edt::live_mode_at_open(asking_for_live(), std::nullopt, k_project));
	BOOST_TEST(warnings == 1u);
	const std::string other_turned_on = edt::with_live_consent(std::nullopt, k_other, true);
	BOOST_TEST(!edt::live_mode_at_open(asking_for_live(), other_turned_on, k_project));
	BOOST_TEST(!edt::live_mode_at_open(edt::build_settings{}, std::nullopt, k_project));
	BOOST_TEST(warnings == 2u);
}

// #908: what the person turned on for this project is on at its next opening, whatever build.json says.
BOOST_AUTO_TEST_CASE(ThePersonsSwitchIsKeptPerProject)
{
	std::string text = edt::with_live_consent(std::nullopt, k_project, true);
	BOOST_TEST(edt::live_consented(text, k_project));
	BOOST_TEST(!edt::live_consented(text, k_other));
	BOOST_TEST(edt::live_mode_at_open(edt::build_settings{}, text, k_project));
	BOOST_TEST(edt::live_mode_at_open(asking_for_live(), text, k_project));

	text = edt::with_live_consent(text, k_other, true);
	text = edt::with_live_consent(text, k_project, false);
	BOOST_TEST(!edt::live_consented(text, k_project));
	BOOST_TEST(edt::live_consented(text, k_other));
}

// Settings that are not ours to read give no consent, and the next switch starts them afresh.
BOOST_AUTO_TEST_CASE(UnreadableSettingsGiveNoConsent)
{
	for (const std::string bad : { "", "not json", "[]", R"({"projects":[]})", R"({"projects":{"C:/work/game":1}})",
			R"({"projects":{"C:/work/game":"true"}})" }) {
		BOOST_TEST_CONTEXT(bad) {
			BOOST_TEST(!edt::live_consented(bad, k_project));
			BOOST_TEST(edt::live_consented(edt::with_live_consent(bad, k_project, true), k_project));
		}
	}
	const std::string kept = edt::with_live_consent(std::string{ R"({"note":"mine","projects":{}})" }, k_project, true);
	BOOST_TEST(kept.find(R"("note":"mine")") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
