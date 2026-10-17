#include <boost/test/unit_test.hpp>
#include "logger/engine_log.h"
#include "test_temp_dir.hpp"
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace {

struct captured_line
{
	std::string category;
	engine::log_level level;
	std::string message;
};

// Installs a capturing callback and always removes it again, so later tests never log into a
// dangling lambda.
struct capture
{
	capture()
	{
		engine::set_log_callback([this](std::string_view cat, engine::log_level lvl, std::string_view msg) {
			lines.push_back({ std::string(cat), lvl, std::string(msg) });
		});
	}
	~capture() { engine::set_log_callback(nullptr); }
	std::vector<captured_line> lines;
};

}

BOOST_AUTO_TEST_SUITE(EngineLogTests)

BOOST_AUTO_TEST_CASE(Callback_ReceivesCategoryLevelAndFormattedMessage)
{
	capture cap;
	egLOG_WARN("covtest/cb", "x={} y={} z={:.1f}", 1, "two", 3.25);
	BOOST_REQUIRE(cap.lines.size() == 1u);
	BOOST_TEST(cap.lines[0].category == "covtest/cb");
	BOOST_TEST((cap.lines[0].level == engine::log_level::warn));
	BOOST_TEST(cap.lines[0].message == "x=1 y=two z=3.2");
}

BOOST_AUTO_TEST_CASE(Macros_MapToTheirLevels_AndEgLogIsInfo)
{
	capture cap;
	egLOG_TRACE("covtest/lvl", "t");
	egLOG_DEBUG("covtest/lvl", "d");
	egLOG_INFO("covtest/lvl", "i");
	egLOG_WARN("covtest/lvl", "w");
	egLOG_ERROR("covtest/lvl", "e");
	egLOG("covtest/lvl", "plain {}", 7);

	BOOST_REQUIRE(cap.lines.size() == 6u);
	const engine::log_level expected[] = {
		engine::log_level::trace, engine::log_level::debug, engine::log_level::info,
		engine::log_level::warn, engine::log_level::error, engine::log_level::info };
	for (size_t i = 0; i < 6; ++i) BOOST_TEST((cap.lines[i].level == expected[i]));
	BOOST_TEST(cap.lines[5].message == "plain 7");
}

BOOST_AUTO_TEST_CASE(Callback_SeesMessagesEvenWhenTheCategoryLevelFiltersThemFromSinks)
{
	// Pins current behavior: the editor-console callback is not subject to level filtering.
	capture cap;
	engine::set_category_level("covtest/quiet", engine::log_level::error);
	egLOG_DEBUG("covtest/quiet", "hidden from sinks");
	BOOST_REQUIRE(cap.lines.size() == 1u);
	BOOST_TEST(cap.lines[0].message == "hidden from sinks");
}

BOOST_AUTO_TEST_CASE(Callback_RemovedByPassingNull)
{
	std::vector<std::string> seen;
	engine::set_log_callback([&](std::string_view, engine::log_level, std::string_view m) { seen.emplace_back(m); });
	egLOG_INFO("covtest/cb2", "one");
	engine::set_log_callback(nullptr);
	egLOG_INFO("covtest/cb2", "two");
	BOOST_TEST((seen == std::vector<std::string>{ "one" }));
}

BOOST_AUTO_TEST_CASE(CategoryFile_ReceivesOnlyItsCategoryWithFormattedLine)
{
	test_util::temp_dir dir("engine_log");
	auto file_a = dir.path() / "a.log";
	auto file_b = dir.path() / "b.log";
	engine::set_category_file("covtest/file_a", file_a.string());
	engine::set_category_file("covtest/file_b", file_b.string());

	egLOG_WARN("covtest/file_a", "hello {}", "file");   // warn+ is flushed immediately

	auto slurp = [](const std::filesystem::path& p) {
		std::ifstream in(p);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	};
	auto a = slurp(file_a);
	BOOST_TEST(a.find("[warning] [covtest/file_a] hello file") != std::string::npos);
	BOOST_TEST(slurp(file_b).find("hello") == std::string::npos);
	// (the sinks keep the files open until process exit; temp_dir removal ignores that on POSIX)
}

BOOST_AUTO_TEST_CASE(LoggingAfterShutdownReinitializes)
{
	capture cap;
	engine::shutdown_logger();
	capture cap2;
	BOOST_CHECK_NO_THROW(egLOG_INFO("covtest/after_shutdown", "still alive"));
	BOOST_TEST(cap2.lines.size() == 1u);
}

BOOST_AUTO_TEST_CASE(GlobalLevel_CanBeRaisedAndRestoredWithoutAffectingCallbackDelivery)
{
	capture cap;
	engine::set_log_level(engine::log_level::error);
	egLOG_INFO("covtest/global", "filtered by sinks");
	engine::set_log_level(engine::log_level::info);
	egLOG_INFO("covtest/global", "delivered");
	BOOST_TEST(cap.lines.size() == 2u);
}

BOOST_AUTO_TEST_SUITE_END()
