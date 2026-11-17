#include <boost/test/unit_test.hpp>
#include <snk/common/logger/engine_log.h>
#include "test_temp_dir.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// The log file is named late (#713): the process starts with none, and what it logged before is written when one is chosen.

namespace {

std::string read_file(const std::filesystem::path& file)
{
	std::ifstream in(file, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

// The test process's logger is shared: the file is dropped again whatever the test did.
struct log_file_guard
{
	~log_file_guard() { engine::set_log_file(""); }
};

}

BOOST_AUTO_TEST_SUITE(EngineLogFileTests)

BOOST_AUTO_TEST_CASE(AFileChosenLate_GetsTheLinesLoggedBeforeIt_AndTheOnesAfter)
{
	engine::init_logger();
	test_util::temp_dir dir("engine_log_file_late");
	log_file_guard guard;
	const auto previous_capacity = engine::log_journal_capacity();
	engine::set_log_journal_capacity(1000);

	egLOG_WARN("test", "line before the file 713-a");
	const std::filesystem::path file = dir.path() / "logs" / "engine.log";
	engine::set_log_file(file.string());
	egLOG_WARN("test", "line after the file 713-b");
	engine::set_log_file("");

	const std::string text = read_file(file);
	BOOST_CHECK_MESSAGE(text.find("line before the file 713-a") != std::string::npos, text);
	BOOST_CHECK_MESSAGE(text.find("line after the file 713-b") != std::string::npos, text);
	engine::set_log_journal_capacity(previous_capacity);
}

BOOST_AUTO_TEST_CASE(AnEmptyPath_DropsTheFile_SoLaterLinesAreNotWrittenToIt)
{
	engine::init_logger();
	test_util::temp_dir dir("engine_log_file_dropped");
	log_file_guard guard;
	const std::filesystem::path file = dir.path() / "engine.log";

	engine::set_log_file(file.string());
	egLOG_WARN("test", "while the file is there 713-c");
	engine::set_log_file("");
	egLOG_WARN("test", "after the file is dropped 713-d");

	const std::string text = read_file(file);
	BOOST_CHECK_MESSAGE(text.find("while the file is there 713-c") != std::string::npos, text);
	BOOST_CHECK_MESSAGE(text.find("after the file is dropped 713-d") == std::string::npos, text);
}

BOOST_AUTO_TEST_CASE(TheSamePathChosenAgain_DoesNotRepeatTheLinesAlreadyInTheFile)
{
	engine::init_logger();
	test_util::temp_dir dir("engine_log_file_again");
	log_file_guard guard;
	const auto previous_capacity = engine::log_journal_capacity();
	engine::set_log_journal_capacity(1000);
	const std::filesystem::path file = dir.path() / "engine.log";

	egLOG_WARN("test", "once only 713-f");
	engine::set_log_file(file.string());
	engine::set_log_file(file.string());
	engine::set_log_file("");

	const std::string text = read_file(file);
	const auto first = text.find("once only 713-f");
	BOOST_CHECK_MESSAGE(first != std::string::npos, text);
	BOOST_CHECK_MESSAGE(text.find("once only 713-f", first + 1) == std::string::npos, text);
	engine::set_log_journal_capacity(previous_capacity);
}

BOOST_AUTO_TEST_CASE(TheLinesThatDoNotFit_AreCountedInOneLine)
{
	engine::init_logger();
	test_util::temp_dir dir("engine_log_file_counted");
	log_file_guard guard;
	const auto previous_capacity = engine::log_journal_capacity();
	engine::set_log_journal_capacity(2);

	for (int i = 0; i < 5; ++i) {
		egLOG_WARN("test", "numbered line {} 713-e", i);
	}
	const std::filesystem::path file = dir.path() / "engine.log";
	engine::set_log_file(file.string());
	engine::set_log_file("");

	const std::string text = read_file(file);
	BOOST_CHECK_MESSAGE(text.find("earlier log lines are not in this file") != std::string::npos, text);
	BOOST_CHECK_MESSAGE(text.find("numbered line 4 713-e") != std::string::npos, text);
	BOOST_CHECK_MESSAGE(text.find("numbered line 0 713-e") == std::string::npos, text);
	engine::set_log_journal_capacity(previous_capacity);
}

BOOST_AUTO_TEST_SUITE_END()
