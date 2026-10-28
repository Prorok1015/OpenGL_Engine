#include <boost/test/unit_test.hpp>
#include <snk/platform/plt_process.h>
#include "test_temp_dir.hpp"
#include <chrono>
#include <cstdint>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#endif

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

	// Until the file appears, at most `limit`: a child that never writes it fails the test instead of hanging the run.
	bool wait_for_file(const fs::path& file, std::chrono::seconds limit)
	{
		const auto deadline = std::chrono::steady_clock::now() + limit;
		while (std::chrono::steady_clock::now() < deadline) {
			if (fs::exists(file)) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		return fs::exists(file);
	}

	// Ends the child (if it still runs) and waits for it, so no test leaves a process behind.
	void stop_child(std::uint64_t process_id, const fs::path& program)
	{
#ifdef _WIN32
		HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
			static_cast<DWORD>(process_id));
		if (process == nullptr) {
			return; // already gone
		}
		// An id is reused once its process is gone: only our own program is ended.
		wchar_t image[32768];
		DWORD length = static_cast<DWORD>(std::size(image));
		std::error_code ec;
		if (QueryFullProcessImageNameW(process, 0, image, &length) && fs::equivalent(fs::path{ image }, program, ec)) {
			TerminateProcess(process, 1);
			WaitForSingleObject(process, 30000);
		}
		CloseHandle(process);
#else
		(void)program;
		// Our child: its id stays reserved (a zombie) until waitpid below, so the signal cannot reach another process.
		kill(static_cast<pid_t>(process_id), SIGKILL);
		waitpid(static_cast<pid_t>(process_id), nullptr, 0);
#endif
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

	const fs::path program = platform::current_executable();

	const auto started = platform::spawn_detached(program, { "--run_test=PltProcessTests/SpawnHelper_WritesTheMarker",
		"--", std::string{ k_marker_flag }, std::string{ utf8.begin(), utf8.end() } });

	BOOST_REQUIRE_MESSAGE(static_cast<bool>(started), started.error);
	BOOST_TEST(started.process_id != 0u);
	// Sanitizer builds start slowly.
	BOOST_TEST(wait_for_file(marker, std::chrono::seconds{ 120 }));
	stop_child(started.process_id, program);
}

BOOST_AUTO_TEST_CASE(SpawnDetached_AMissingProgramIsAnError)
{
	test_util::temp_dir dir("plt missing");

	const auto started = platform::spawn_detached(dir.path() / "no_such_program", {});

	BOOST_TEST(!started);
	BOOST_TEST(started.process_id == 0u);
	BOOST_TEST(started.error.find("no_such_program") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
