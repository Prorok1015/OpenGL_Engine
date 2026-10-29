#include <boost/test/unit_test.hpp>
#include "edt_build_settings.h"
#include "test_temp_dir.hpp"
#include <fstream>
#include <iterator>
#include <snk/common/logger/engine_log_sinks.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The per-project build settings (<root>/.snk/build.json, #444).

namespace {

struct warn_log
{
	warn_log()
	{
		handle = engine::add_log_sink([this](std::string_view, engine::log_level level, std::string_view message) {
			if (level != engine::log_level::warn) return;
			const std::lock_guard lock(mutex);
			lines.emplace_back(message);
		});
	}

	bool has(std::string_view text)
	{
		const std::lock_guard lock(mutex);
		return std::any_of(lines.begin(), lines.end(), [text](const std::string& line) { return line.find(text) != std::string::npos; });
	}

	std::size_t size()
	{
		const std::lock_guard lock(mutex);
		return lines.size();
	}

	std::mutex mutex;
	std::vector<std::string> lines;
	engine::log_sink_handle handle;
};

bool same(const edt::build_settings& a, const edt::build_settings& b)
{
	return a == b;
}

}

BOOST_AUTO_TEST_SUITE(EdtBuildSettingsTests)

BOOST_AUTO_TEST_CASE(Defaults)
{
	const edt::build_settings settings;
	BOOST_CHECK(!settings.configuration.has_value());
	BOOST_CHECK(settings.jobs_mode == edt::jobs_mode::auto_jobs);
	BOOST_CHECK(settings.reload_after_build);
	BOOST_CHECK(!settings.build_before_play);
	BOOST_CHECK(settings.log_verbosity == edt::log_verbosity::normal);

	const auto parsed = edt::parse_build_settings("{}");
	BOOST_CHECK(parsed.warnings.empty());
	BOOST_CHECK(same(parsed.settings, settings));
}

BOOST_AUTO_TEST_CASE(Parse_ReadsEveryKey_AndIgnoresUnknownOnes)
{
	const auto parsed = edt::parse_build_settings(R"({"configuration":"release","jobs_mode":"manual","reload_after_build":false,
		"build_before_play":true,"log_verbosity":"verbose","future_key":{"x":1}})");
	BOOST_CHECK(parsed.warnings.empty());
	BOOST_CHECK(parsed.settings.configuration == std::optional<std::string>{ "release" });
	BOOST_CHECK(parsed.settings.jobs_mode == edt::jobs_mode::manual_jobs);
	BOOST_CHECK(!parsed.settings.reload_after_build);
	BOOST_CHECK(parsed.settings.build_before_play);
	BOOST_CHECK(parsed.settings.log_verbosity == edt::log_verbosity::verbose);
}

BOOST_AUTO_TEST_CASE(Merge_RoundTrips_AndKeepsUnknownKeysInOrder)
{
	boost::json::value doc = boost::json::parse(R"({"note":"mine","configuration":"debuggame","jobs_mode":"auto","extra":[1,2]})");
	edt::build_settings settings;
	settings.jobs_mode = edt::jobs_mode::manual_jobs;
	settings.reload_after_build = false;
	settings.build_before_play = true;
	settings.log_verbosity = edt::log_verbosity::errors;
	edt::merge_build_settings(doc, settings);

	const auto& object = doc.as_object();
	BOOST_CHECK(object.at("note").as_string() == "mine");
	BOOST_CHECK(object.at("extra") == boost::json::parse("[1,2]"));
	BOOST_CHECK(!object.contains("configuration")); // unset: cfg build/configuration decides
	std::vector<std::string> keys;
	for (const auto& entry : object) keys.emplace_back(entry.key());
	BOOST_REQUIRE_GE(keys.size(), 3u);
	BOOST_CHECK_EQUAL(keys[0], "note");
	BOOST_CHECK_EQUAL(keys[1], "jobs_mode");
	BOOST_CHECK_EQUAL(keys[2], "extra");

	const auto parsed = edt::parse_build_settings(boost::json::serialize(doc));
	BOOST_CHECK(parsed.warnings.empty());
	BOOST_CHECK(same(parsed.settings, settings));

	settings.configuration = "release";
	edt::merge_build_settings(doc, settings);
	BOOST_CHECK(same(edt::parse_build_settings(boost::json::serialize(doc)).settings, settings));
}

BOOST_AUTO_TEST_CASE(Merge_IntoANonObject_MakesAnObject)
{
	boost::json::value doc = boost::json::parse("[1]");
	edt::merge_build_settings(doc, {});
	BOOST_REQUIRE(doc.is_object());
	BOOST_CHECK(doc.as_object().at("log_verbosity").as_string() == "normal");
	BOOST_CHECK(doc.as_object().at("jobs_mode").as_string() == "auto");
}

BOOST_AUTO_TEST_CASE(BadValues_WarnAndKeepTheDefaults)
{
	const auto parsed = edt::parse_build_settings(R"({"configuration":"Debug","jobs_mode":4,"reload_after_build":"yes",
		"build_before_play":1,"log_verbosity":"loud"})");
	BOOST_CHECK_EQUAL(parsed.warnings.size(), 5u);
	BOOST_CHECK(same(parsed.settings, edt::build_settings{}));
	for (const std::string key : { "configuration", "jobs_mode", "reload_after_build", "build_before_play", "log_verbosity" }) {
		BOOST_CHECK_MESSAGE(std::any_of(parsed.warnings.begin(), parsed.warnings.end(),
			[&key](const std::string& warning) { return warning.find(key) != std::string::npos; }), key);
	}

}

BOOST_AUTO_TEST_CASE(NullValues_AreTheDefaultsSilently)
{
	const auto parsed = edt::parse_build_settings(R"({"configuration":null,"jobs_mode":null,"reload_after_build":null,
		"build_before_play":null,"log_verbosity":null})");
	BOOST_CHECK(parsed.warnings.empty());
	BOOST_CHECK(same(parsed.settings, edt::build_settings{}));
}

BOOST_AUTO_TEST_CASE(NotAnObject_IsOneWarning)
{
	const auto parsed = edt::parse_build_settings("[]");
	BOOST_CHECK_EQUAL(parsed.warnings.size(), 1u);
	BOOST_CHECK(same(parsed.settings, edt::build_settings{}));
}

BOOST_AUTO_TEST_CASE(Load_MissingFile_GivesDefaultsSilently)
{
	test_util::temp_dir dir("edt build settings missing");
	warn_log log;
	const auto settings = edt::load_build_settings(dir.path() / ".snk");
	BOOST_CHECK(same(settings, edt::build_settings{}));
	BOOST_CHECK_EQUAL(log.size(), 0u);
}

BOOST_AUTO_TEST_CASE(Load_ReadsTheFile)
{
	test_util::temp_dir dir("edt build settings read");
	dir.write(".snk/build.json", R"({"configuration":"release","log_verbosity":"errors"})");
	const auto settings = edt::load_build_settings(dir.path() / ".snk");
	BOOST_CHECK(settings.configuration == std::optional<std::string>{ "release" });
	BOOST_CHECK(settings.log_verbosity == edt::log_verbosity::errors);
}

BOOST_AUTO_TEST_CASE(Load_BrokenJson_GivesDefaultsAndWarnsNamingTheFile)
{
	test_util::temp_dir dir("edt build settings broken");
	dir.write(".snk/build.json", R"({"configuration": "release",)");
	warn_log log;
	const auto settings = edt::load_build_settings(dir.path() / ".snk");
	BOOST_CHECK(same(settings, edt::build_settings{}));
	BOOST_CHECK(log.has("build.json"));
}

BOOST_AUTO_TEST_CASE(FreshConfigure_ParsesNamesOnce_AndDropsOnlyBadEntries)
{
	auto parsed = edt::parse_build_settings(R"({"fresh_configure":["debuggame","release","debuggame"]})");
	BOOST_CHECK(parsed.warnings.empty());
	BOOST_CHECK((parsed.settings.fresh_configure == std::vector<std::string>{ "debuggame", "release" }));

	parsed = edt::parse_build_settings(R"({"fresh_configure":["release",4,""]})");
	BOOST_CHECK_EQUAL(parsed.warnings.size(), 2u);
	BOOST_CHECK((parsed.settings.fresh_configure == std::vector<std::string>{ "release" }));

	parsed = edt::parse_build_settings(R"({"fresh_configure":"release"})");
	BOOST_CHECK_EQUAL(parsed.warnings.size(), 1u);
	BOOST_CHECK(parsed.settings.fresh_configure.empty());

	parsed = edt::parse_build_settings(R"({"fresh_configure":null})");
	BOOST_CHECK(parsed.warnings.empty());
	BOOST_CHECK(parsed.settings.fresh_configure.empty());
}

BOOST_AUTO_TEST_CASE(FreshConfigure_MergeWritesTheList_AndDropsTheKeyWhenEmpty)
{
	boost::json::value doc = boost::json::parse(R"({"note":1})");
	edt::build_settings settings;
	settings.fresh_configure = { "release" };
	edt::merge_build_settings(doc, settings);
	BOOST_CHECK(doc.as_object().at("fresh_configure") == boost::json::parse(R"(["release"])"));
	BOOST_CHECK(same(edt::parse_build_settings(boost::json::serialize(doc)).settings, settings));

	settings.fresh_configure.clear();
	edt::merge_build_settings(doc, settings);
	BOOST_CHECK(!doc.as_object().contains("fresh_configure"));
	BOOST_CHECK(doc.as_object().at("note").as_int64() == 1);
}

BOOST_AUTO_TEST_CASE(Update_CreatesTheFolder_KeepsUnknownKeys_AndWritesNothingWithoutAChange)
{
	test_util::temp_dir dir("edt build settings update");
	const auto snk = dir.path() / ".snk";
	BOOST_CHECK(!edt::update_build_settings(snk, [](edt::build_settings&) {}).has_value());
	BOOST_CHECK(!std::filesystem::exists(snk / "build.json"));

	BOOST_REQUIRE(!edt::update_build_settings(snk, [](edt::build_settings& settings) {
		settings.fresh_configure.push_back("debuggame");
	}).has_value());
	BOOST_CHECK((edt::load_build_settings(snk).fresh_configure == std::vector<std::string>{ "debuggame" }));

	dir.write(".snk/build.json", R"({"mine":true,"fresh_configure":["debuggame","release"]})");
	BOOST_REQUIRE(!edt::clear_fresh_configure(snk, "debuggame").has_value());
	const auto doc = boost::json::parse([&] {
		std::ifstream in(snk / "build.json", std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}());
	BOOST_CHECK(doc.as_object().at("mine").as_bool());
	BOOST_CHECK(doc.as_object().at("fresh_configure") == boost::json::parse(R"(["release"])"));
}

BOOST_AUTO_TEST_CASE(Update_ABrokenFileIsAnErrorAndStaysAsItIs)
{
	test_util::temp_dir dir("edt build settings update broken");
	constexpr std::string_view broken = R"({"fresh_configure": ["debuggame")";
	dir.write(".snk/build.json", broken);
	BOOST_CHECK(edt::check_build_settings_file(dir.path() / ".snk").has_value());
	BOOST_CHECK(edt::clear_fresh_configure(dir.path() / ".snk", "debuggame").has_value());
	std::ifstream in(dir.path() / ".snk" / "build.json", std::ios::binary);
	BOOST_CHECK(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) == broken);

	dir.write(".snk/build.json", "[]");
	BOOST_CHECK(edt::check_build_settings_file(dir.path() / ".snk").has_value());
}

BOOST_AUTO_TEST_SUITE_END()
