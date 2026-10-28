#include <boost/test/unit_test.hpp>
#include <snk/platform/plt_process.h>
#include "test_temp_dir.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

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
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

// spawn_detached closes the inherited descriptors where glibc can (posix_spawn_file_actions_addclosefrom_np).
#if defined(__linux__) && defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
#define SNK_TEST_SPAWN_CLOSES_FDS 1
#endif
#endif

// snk::platform (EPIC-42 #158): what the editor needs to restart itself into another project.

namespace {
	namespace fs = std::filesystem;

	constexpr std::string_view k_marker_flag = "--plt-marker";
	constexpr std::string_view k_fd_flag = "--plt-fd";
	constexpr std::string_view k_talk_flag = "--plt-talk";
	constexpr std::string_view k_late_marker_flag = "--plt-late-marker";
	constexpr std::string_view k_tree_flag = "--plt-tree";
	constexpr std::string_view k_test_variable = "SNK_PLT_TEST";

	std::string argument_after(std::string_view flag)
	{
		const auto& suite = boost::unit_test::framework::master_test_suite();
		for (int i = 0; i + 1 < suite.argc; ++i) {
			if (suite.argv[i] == flag) {
				return suite.argv[i + 1];
			}
		}
		return {};
	}

	bool has_argument(std::string_view flag)
	{
		const auto& suite = boost::unit_test::framework::master_test_suite();
		for (int i = 0; i < suite.argc; ++i) {
			if (suite.argv[i] == flag) {
				return true;
			}
		}
		return false;
	}

	// The marker path when this run is a child started by one of the SpawnDetached tests.
	std::string marker_argument()
	{
		return argument_after(k_marker_flag);
	}

	// This run is a child started by a test here: the tests that start children must not start more.
	bool is_child_run()
	{
		return !marker_argument().empty() || has_argument(k_talk_flag) || has_argument(k_late_marker_flag)
			|| has_argument(k_tree_flag);
	}

	fs::path path_of_utf8(const std::string& utf8)
	{
		return fs::path{ std::u8string{ utf8.begin(), utf8.end() } };
	}

	std::string utf8_of(const fs::path& path)
	{
		const std::u8string text = path.u8string();
		return { text.begin(), text.end() };
	}

	// Written aside and renamed, so a marker that exists is complete.
	void write_marker(const fs::path& file, const std::string& text)
	{
		fs::path part = file;
		part += ".part";
		std::ofstream{ part } << text;
		fs::rename(part, file);
	}

	void print_line(std::FILE* stream, std::string_view line)
	{
		const std::string text = std::string{ line } + "\n";
		std::fputs(text.c_str(), stream);
		std::fflush(stream);
	}

	// std::getenv is deprecated on MSVC (C4996).
	std::string environment_value(std::string_view name)
	{
		const std::string key{ name };
#ifdef _MSC_VER
		char* buffer = nullptr;
		std::size_t length = 0;
		if (_dupenv_s(&buffer, &length, key.c_str()) != 0 || buffer == nullptr) return {};
		std::string value(buffer);
		std::free(buffer);
		return value;
#else
		const char* value = std::getenv(key.c_str());
		return value ? std::string(value) : std::string();
#endif
	}

	std::vector<std::string> parent_environment()
	{
		std::vector<std::string> out;
#ifdef _WIN32
		wchar_t* block = GetEnvironmentStringsW();
		if (block == nullptr) return out;
		for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wstring_view{ entry }.size() + 1) {
			out.push_back(utf8_of(fs::path{ std::wstring_view{ entry } }));
		}
		FreeEnvironmentStringsW(block);
#else
		for (char** entry = environ; *entry != nullptr; ++entry) {
			out.emplace_back(*entry);
		}
#endif
		return out;
	}

	bool contains_in_order(const std::vector<std::string>& lines, std::initializer_list<std::string_view> wanted)
	{
		auto it = lines.begin();
		for (const std::string_view line : wanted) {
			it = std::find(it, lines.end(), line);
			if (it == lines.end()) return false;
			++it;
		}
		return true;
	}

	std::string joined(const std::vector<std::string>& lines)
	{
		std::string out;
		for (const auto& line : lines) {
			out += line;
			out += " | ";
		}
		return out;
	}

	// The lines of this test program run as a child with `args`.
	platform::process_result run_self(const std::vector<std::string>& args, const platform::process_options& options,
		std::vector<std::string>& lines)
	{
		return platform::run_process(platform::current_executable(), args, options,
			[&lines](std::string_view line) { lines.emplace_back(line); }, {});
	}

	std::string read_text(const fs::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
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

// What CommandLineToArgvW gives back is the argument as given (MSDN, "Everyone quotes command line arguments the
// wrong way").
BOOST_AUTO_TEST_CASE(QuoteWindowsArg_FollowsTheCommandLineToArgvWRules)
{
	struct quoting { std::wstring_view arg; std::wstring_view quoted; };
	for (const quoting& q : {
			quoting{ L"plain", L"plain" },
			quoting{ L"", L"\"\"" },
			quoting{ L"with space", L"\"with space\"" },
			quoting{ L"tab\there", L"\"tab\there\"" },
			quoting{ L"say \"hi\"", L"\"say \\\"hi\\\"\"" },
			quoting{ LR"(C:\dir\file)", LR"(C:\dir\file)" },        // backslashes alone stay as they are
			quoting{ LR"(trailing\)", LR"(trailing\)" },             // no quotes needed: nothing to double
			quoting{ LR"(a dir\)", LR"("a dir\\")" },               // before the closing quote: doubled
			quoting{ LR"(a\"b)", LR"("a\\\"b")" },                 // \" -> \\\" inside quotes
			quoting{ LR"(a\\"b c)", LR"("a\\\\\"b c")" } }) {
		BOOST_TEST_INFO("arg #" << q.arg.size());
		BOOST_TEST((platform::quote_windows_arg(q.arg) == q.quoted));
	}
}

BOOST_AUTO_TEST_CASE(CurrentExecutable_IsThisTestProgram)
{
	const fs::path exe = platform::current_executable();

	BOOST_TEST(exe.is_absolute());
	BOOST_TEST(fs::is_regular_file(exe));
	BOOST_TEST(exe.stem().string().find("unit_tests") != std::string::npos);
}

// Does nothing in a normal run; in the child it writes the file named after --plt-marker: "spawned", or with --plt-fd
// what that descriptor is open on ("closed" if it is not open). Written aside and renamed, so a marker that exists is
// complete.
BOOST_AUTO_TEST_CASE(SpawnHelper_WritesTheMarker)
{
	const std::string marker = marker_argument();
	if (marker.empty()) {
		return;
	}
	std::string text = "spawned";
	if (const std::string fd = argument_after(k_fd_flag); !fd.empty()) {
		std::error_code ec;
		const fs::path target = fs::read_symlink(fs::path{ "/proc/self/fd" } / fd, ec);
		text = ec ? std::string{ "closed" } : target.string();
	}
	const fs::path file{ std::u8string{ marker.begin(), marker.end() } };
	fs::path part = file;
	part += ".part";
	std::ofstream{ part } << text;
	fs::rename(part, file);
}

BOOST_AUTO_TEST_CASE(SpawnDetached_RunsTheProgramWithItsArguments)
{
	if (is_child_run()) {
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

#if SNK_TEST_SPAWN_CLOSES_FDS
// A descriptor without close-on-exec (as Asio opens the MCP listening socket) must not reach the new editor.
BOOST_AUTO_TEST_CASE(SpawnDetached_DoesNotHandDownOpenDescriptors)
{
	if (is_child_run()) {
		return;
	}
	test_util::temp_dir dir("plt fds");
	const fs::path witness = dir.write("witness.txt", "x");
	const fs::path marker = dir.path() / "marker.txt";

	const int opened = ::open(witness.c_str(), O_RDONLY);
	BOOST_REQUIRE(opened >= 0);
	// High above what the child opens while it starts, so a reused number cannot pass for the inherited one.
	const int inheritable = ::fcntl(opened, F_DUPFD, 200);
	::close(opened);
	BOOST_REQUIRE(inheritable >= 0);
	BOOST_REQUIRE((::fcntl(inheritable, F_GETFD) & FD_CLOEXEC) == 0);

	const fs::path program = platform::current_executable();
	const auto started = platform::spawn_detached(program, { "--run_test=PltProcessTests/SpawnHelper_WritesTheMarker",
		"--", std::string{ k_marker_flag }, marker.string(), std::string{ k_fd_flag }, std::to_string(inheritable) });
	::close(inheritable);

	BOOST_REQUIRE_MESSAGE(static_cast<bool>(started), started.error);
	BOOST_TEST(wait_for_file(marker, std::chrono::seconds{ 120 }));
	const std::string seen = read_text(marker);
	BOOST_TEST(!seen.empty());
	BOOST_TEST_INFO("descriptor in the child: " << seen);
	BOOST_TEST(seen.find("witness.txt") == std::string::npos);
	stop_child(started.process_id, program);
}
#endif

BOOST_AUTO_TEST_CASE(SpawnDetached_AMissingProgramIsAnError)
{
	test_util::temp_dir dir("plt missing");

	const auto started = platform::spawn_detached(dir.path() / "no_such_program", {});

	BOOST_TEST(!started);
	BOOST_TEST(started.process_id == 0u);
	BOOST_TEST(started.error.find("no_such_program") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TakeLines_SplitsOnLfAndCrLfAcrossChunks)
{
	std::vector<std::string> lines;
	const auto collect = [&lines](std::string_view line) { lines.emplace_back(line); };
	std::string pending;

	platform::detail::take_lines(pending, "one\r\ntw", false, collect);
	platform::detail::take_lines(pending, "o\r", false, collect);
	platform::detail::take_lines(pending, "\n\nthree\nlast", false, collect);
	BOOST_TEST(pending == "last");
	platform::detail::take_lines(pending, {}, true, collect);

	const std::vector<std::string> expected{ "one", "two", "", "three", "last" };
	BOOST_TEST(lines == expected, boost::test_tools::per_element());
	BOOST_TEST(pending.empty());
}

// Does nothing in a normal run; with --plt-talk it writes known lines to stdout and stderr, the value of SNK_PLT_TEST
// and its working directory, a last line without a break, and exits with 7.
BOOST_AUTO_TEST_CASE(RunHelper_Talks)
{
	if (!has_argument(k_talk_flag)) {
		return;
	}
	print_line(stdout, "out one");
	print_line(stderr, "err one");
	print_line(stdout, "out two");
	print_line(stdout, "env=" + environment_value(k_test_variable));
	print_line(stdout, "cwd=" + utf8_of(fs::current_path()));
	std::fputs("err tail", stderr);
	std::fflush(stderr);
	// Not std::exit: no atexit handlers (the leak check of a sanitizer build could change the exit code).
	std::_Exit(7);
}

// Does nothing in a normal run; with --plt-late-marker it says "grandchild ready" and writes the marker 3 s later.
BOOST_AUTO_TEST_CASE(RunHelper_WritesTheMarkerLate)
{
	const std::string marker = argument_after(k_late_marker_flag);
	if (marker.empty()) {
		return;
	}
	print_line(stdout, "grandchild ready");
	std::this_thread::sleep_for(std::chrono::seconds{ 3 });
	write_marker(path_of_utf8(marker), "late");
}

// Does nothing in a normal run; with --plt-tree it starts RunHelper_WritesTheMarkerLate (whose lines reach this
// process's output) and waits until it is ended.
BOOST_AUTO_TEST_CASE(RunHelper_StartsAGrandchild)
{
	const std::string marker = argument_after(k_tree_flag);
	if (marker.empty()) {
		return;
	}
	const std::vector<std::string> args{ "--run_test=PltProcessTests/RunHelper_WritesTheMarkerLate", "--",
		std::string{ k_late_marker_flag }, marker };
#ifdef _WIN32
	// In a job nested in the parent's, as a build tool that runs its own children under a job.
	std::jthread grandchild([&args](std::stop_token stop) {
		platform::run_process(platform::current_executable(), args, {},
			[](std::string_view line) { print_line(stdout, line); }, stop);
	});
	std::this_thread::sleep_for(std::chrono::seconds{ 120 });
#else
	// In this process's group, as a compiler under make: run_process would give it a group of its own.
	std::string program = platform::current_executable().string();
	std::vector<std::string> owned = args;
	std::vector<char*> argv;
	argv.reserve(owned.size() + 2);
	argv.push_back(program.data());
	for (auto& arg : owned) {
		argv.push_back(arg.data());
	}
	argv.push_back(nullptr);
	pid_t pid = 0;
	if (posix_spawn(&pid, program.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
		return;
	}
	std::this_thread::sleep_for(std::chrono::seconds{ 120 });
	waitpid(pid, nullptr, 0);
#endif
}

BOOST_AUTO_TEST_CASE(RunProcess_DeliversTheLinesOfStdoutAndStderrAndTheExitCode)
{
	if (is_child_run()) {
		return;
	}
	std::vector<std::string> lines;

	const auto result = run_self({ "--run_test=PltProcessTests/RunHelper_Talks", "--", std::string{ k_talk_flag } },
		{}, lines);

	BOOST_REQUIRE_MESSAGE(result.error.empty(), result.error);
	BOOST_TEST_INFO("output: " << joined(lines));
	BOOST_TEST(!result.cancelled);
	BOOST_TEST(result.exit_code == 7);
	BOOST_TEST(!result);
	BOOST_TEST(contains_in_order(lines, { "out one", "err one", "out two" }));
	BOOST_TEST(std::ranges::any_of(lines, [](const std::string& line) { return line.starts_with("err tail"); }));
}

BOOST_AUTO_TEST_CASE(RunProcess_PassesTheEnvironmentAndTheWorkingDirectory)
{
	if (is_child_run()) {
		return;
	}
	test_util::temp_dir dir("plt run env");
	platform::process_options options;
	options.working_dir = dir.path();
	options.environment = parent_environment();
	options.environment->push_back(std::string{ k_test_variable } + "=hello from the parent");
	std::vector<std::string> lines;

	const auto result = run_self({ "--run_test=PltProcessTests/RunHelper_Talks", "--", std::string{ k_talk_flag } },
		options, lines);

	BOOST_REQUIRE_MESSAGE(result.error.empty(), result.error);
	BOOST_TEST_INFO("output: " << joined(lines));
	BOOST_TEST(result.exit_code == 7);
	BOOST_TEST(contains_in_order(lines, { "env=hello from the parent" }));
	const auto cwd = std::ranges::find_if(lines, [](const std::string& line) { return line.starts_with("cwd="); });
	BOOST_REQUIRE(cwd != lines.end());
	std::error_code ec;
	BOOST_TEST(fs::equivalent(path_of_utf8(cwd->substr(4)), dir.path(), ec));
}

BOOST_AUTO_TEST_CASE(RunProcess_StopEndsTheWholeProcessTree)
{
	if (is_child_run()) {
		return;
	}
	test_util::temp_dir dir("plt run tree");
	const fs::path marker = dir.path() / "grandchild.txt";
	std::stop_source stop;
	bool grandchild_ready = false;

	// The child stays until it is ended (120 s at most), so the stop comes only from here.
	const auto result = platform::run_process(platform::current_executable(),
		{ "--run_test=PltProcessTests/RunHelper_StartsAGrandchild", "--", std::string{ k_tree_flag }, utf8_of(marker) },
		{},
		[&](std::string_view line) {
			if (line.find("grandchild ready") != std::string_view::npos) {
				grandchild_ready = true;
				stop.request_stop();
			}
		},
		stop.get_token());

	BOOST_REQUIRE_MESSAGE(result.error.empty(), result.error);
	BOOST_TEST(grandchild_ready);
	BOOST_TEST(result.cancelled);
	BOOST_TEST(!result);
	// A grandchild that survived writes its marker 3 s after it was ready.
	std::this_thread::sleep_for(std::chrono::seconds{ 6 });
	BOOST_TEST(!fs::exists(marker));
}

BOOST_AUTO_TEST_CASE(RunProcess_AMissingProgramIsAnError)
{
	if (is_child_run()) {
		return;
	}
	test_util::temp_dir dir("plt run missing");
	bool any_line = false;

	const auto result = platform::run_process(dir.path() / "no_such_program", {}, {},
		[&any_line](std::string_view) { any_line = true; }, {});

	BOOST_TEST(!result);
	BOOST_TEST(result.exit_code == -1);
	BOOST_TEST(!result.cancelled);
	BOOST_TEST(result.error.find("no_such_program") != std::string::npos);
	BOOST_TEST(!any_line);
}

BOOST_AUTO_TEST_SUITE_END()
