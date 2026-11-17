#include <boost/test/unit_test.hpp>
#include <snk/config/cfg_variables.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// int and float config values that are not numbers or do not fit (#415): a warning, the old value stays, nothing
// throws. Through --set and through yaml; neither needs the once-per-process initialize_configs(path).

namespace {

struct warning_capture
{
	warning_capture()
	{
		sink = engine::add_log_sink([this](std::string_view, engine::log_level level, std::string_view message) {
			if (level != engine::log_level::warn) return;
			std::lock_guard lock(m);
			warnings.emplace_back(message);
		});
	}

	bool mentions(std::string_view a, std::string_view b)
	{
		std::lock_guard lock(m);
		return std::ranges::any_of(warnings, [&](const std::string& w) {
			return w.find(a) != std::string::npos && w.find(b) != std::string::npos;
		});
	}

	std::mutex m;
	std::vector<std::string> warnings;
	engine::log_sink_handle sink;
};

bool run_set(std::string name, std::string value)
{
	static std::string program = "unit_tests";
	static std::string set = "--set";
	std::vector<char*> argv{ program.data(), set.data(), name.data(), value.data() };
	return cfg::initialize_configs(static_cast<int>(argv.size()), argv.data());
}

bool load(const std::string& yaml)
{
	return cfg::load_config_text(yaml, std::filesystem::path{});
}

// Also given to --set, so none starts with '-' (an option to the command line) or is empty.
const std::vector<std::string> k_bad_ints{ "abc", "1.5", "12px", "99999999999" };
const std::vector<std::string> k_bad_floats{ "abc", "1.5f", "1e40" };

}

BOOST_AUTO_TEST_SUITE(CfgNumericTests)

BOOST_AUTO_TEST_CASE(ParseInt_TakesTheWholeTextOnly)
{
	BOOST_TEST(cfg::parse_int("42").value_or(0) == 42);
	BOOST_TEST(cfg::parse_int(" -7 ").value_or(0) == -7);
	BOOST_TEST(!cfg::parse_int("").has_value());
	BOOST_TEST(!cfg::parse_int("-99999999999").has_value());
	for (const auto& bad : k_bad_ints) {
		BOOST_TEST_INFO("value: '" << bad << "'");
		BOOST_TEST(!cfg::parse_int(bad).has_value());
	}
}

BOOST_AUTO_TEST_CASE(ParseFloat_TakesTheWholeTextOnly)
{
	BOOST_TEST(cfg::parse_float("0.25").value_or(0.f) == 0.25f);
	BOOST_TEST(cfg::parse_float("3").value_or(0.f) == 3.f);
	BOOST_TEST(!cfg::parse_float("").has_value());
	BOOST_TEST(!cfg::parse_float("-1e40").has_value());
	for (const auto& bad : k_bad_floats) {
		BOOST_TEST_INFO("value: '" << bad << "'");
		BOOST_TEST(!cfg::parse_float(bad).has_value());
	}
}

BOOST_AUTO_TEST_CASE(Set_ABadInt_WarnsAndKeepsTheValue)
{
	cfg::variable<int> v{ "cfgnum/set_int", 5 };
	for (const auto& bad : k_bad_ints) {
		BOOST_TEST_INFO("value: '" << bad << "'");
		warning_capture capture;
		BOOST_CHECK_NO_THROW(run_set("cfgnum/set_int", bad));
		BOOST_TEST(static_cast<int>(v) == 5);
		BOOST_TEST(capture.mentions("cfgnum/set_int", "'" + bad + "'"));
	}
	run_set("cfgnum/set_int", "8");
	BOOST_TEST(static_cast<int>(v) == 8);
}

BOOST_AUTO_TEST_CASE(Set_ABadFloat_WarnsAndKeepsTheValue)
{
	cfg::variable<float> v{ "cfgnum/set_float", 0.5f };
	for (const auto& bad : k_bad_floats) {
		BOOST_TEST_INFO("value: '" << bad << "'");
		warning_capture capture;
		BOOST_CHECK_NO_THROW(run_set("cfgnum/set_float", bad));
		BOOST_TEST(static_cast<float>(v) == 0.5f);
		BOOST_TEST(capture.mentions("cfgnum/set_float", "'" + bad + "'"));
	}
	run_set("cfgnum/set_float", "2.5");
	BOOST_TEST(static_cast<float>(v) == 2.5f);
}

BOOST_AUTO_TEST_CASE(Yaml_ABadIntOrFloat_WarnsAndKeepsTheValue)
{
	cfg::variable<int> i{ "cfgnum/yaml_int", 5 };
	cfg::variable<float> f{ "cfgnum/yaml_float", 0.5f };
	for (const char* value : { "abc", "99999999999" }) {
		BOOST_TEST_INFO("int value: " << value);
		warning_capture capture;
		BOOST_CHECK_NO_THROW(load(std::string{ "cfgnum:\n  yaml_int: " } + value + "\n"));
		BOOST_TEST(static_cast<int>(i) == 5);
		BOOST_TEST(capture.mentions("cfgnum/yaml_int", value));
	}
	for (const char* value : { "abc", "1e40" }) {
		BOOST_TEST_INFO("float value: " << value);
		warning_capture capture;
		BOOST_CHECK_NO_THROW(load(std::string{ "cfgnum:\n  yaml_float: " } + value + "\n"));
		BOOST_TEST(static_cast<float>(f) == 0.5f);
		BOOST_TEST(capture.mentions("cfgnum/yaml_float", value));
	}
	BOOST_TEST(load("cfgnum:\n  yaml_int: 12\n  yaml_float: 1.25\n"));
	BOOST_TEST(static_cast<int>(i) == 12);
	BOOST_TEST(static_cast<float>(f) == 1.25f);
}

BOOST_AUTO_TEST_SUITE_END()
