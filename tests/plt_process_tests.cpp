#include <boost/test/unit_test.hpp>
#include <snk/platform/plt_process.h>
#include "test_temp_dir.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

// snk::platform (EPIC-42 #158): what the editor needs to restart itself into another project.

namespace {
	namespace fs = std::filesystem;

	constexpr std::string_view k_marker_flag = "--plt-marker";

	// The marker path when this run is the child started by SpawnDetached_RunsTheProgramWithItsArguments.
	std::string marker_argument()
	{
		const auto& suite = boost::unit_test::framework::master_test_suite();
		for (int i = 0; i + 1 < suite.argc; ++i) {
			if (suite.argv[i] == k_marker_flag) {
				return suite.argv[i + 1];
			}
		}
		return {};
	}

	bool wait_for_file(const fs::path& file)
	{
		// Sanitizer builds start slowly.
		for (int i = 0; i < 600; ++i) {
			if (fs::exists(file)) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return false;
	}
}

BOOST_AUTO_TEST_SUITE(PltProcessTests)

BOOST_AUTO_TEST_CASE(CurrentExecutable_IsThisTestProgram)
{
	const fs::path exe = platform::current_executable();

	BOOST_TEST(exe.is_absolute());
	BOOST_TEST(fs::is_regular_file(exe));
	BOOST_TEST(exe.stem().string().find("unit_tests") != std::string::npos);
}

// Does nothing in a normal run; in the child it writes the file named after --plt-marker.
BOOST_AUTO_TEST_CASE(SpawnHelper_WritesTheMarker)
{
	const std::string marker = marker_argument();
	if (marker.empty()) {
		return;
	}
	std::ofstream{ fs::path{ std::u8string{ marker.begin(), marker.end() } } } << "spawned";
}

BOOST_AUTO_TEST_CASE(SpawnDetached_RunsTheProgramWithItsArguments)
{
	if (!marker_argument().empty()) {
		return; // the child must not spawn again
	}
	test_util::temp_dir dir("plt spawn");
	// A space and a quote-free backslash path on Windows: the arguments must arrive as they were given.
	const fs::path marker = dir.path() / "with space" / "marker.txt";
	fs::create_directories(marker.parent_path());
	const std::u8string utf8 = marker.u8string();

	const auto error = platform::spawn_detached(platform::current_executable(), { "--run_test=PltProcessTests/SpawnHelper_WritesTheMarker",
		"--", std::string{ k_marker_flag }, std::string{ utf8.begin(), utf8.end() } });

	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	BOOST_TEST(wait_for_file(marker));
}

BOOST_AUTO_TEST_CASE(SpawnDetached_AMissingProgramIsAnError)
{
	test_util::temp_dir dir("plt missing");

	const auto error = platform::spawn_detached(dir.path() / "no_such_program", {});

	BOOST_REQUIRE(error.has_value());
	BOOST_TEST(error->find("no_such_program") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
