#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>

namespace platform
{
	// The file of the running program; empty when the system cannot tell.
	SNK_API std::filesystem::path current_executable();

	SNK_API std::int64_t current_process_id();
	// Whether a process with this id runs now: true for one this process may not look at, false for an id <= 0. An id
	// the system reused for another program counts as running.
	SNK_API bool process_is_running(std::int64_t pid);

	struct spawn_result
	{
		// The id the system gave the new process; 0 exactly when `error` is set.
		std::uint64_t process_id = 0;
		std::string error;

		explicit operator bool() const { return error.empty(); }
	};

	// Starts `program` with `args` (the program itself not among them, UTF-8) and does not wait for it: the new process
	// outlives this one.
	SNK_API spawn_result spawn_detached(const std::filesystem::path& program, const std::vector<std::string>& args);

	// One argument of a Windows command line, quoted so CommandLineToArgvW (and the C runtime) give it back unchanged:
	// quotes only around an empty argument or one with a space, tab or '"'; inside them a '"' becomes \" and the
	// backslashes right before a '"' (or the closing one) are doubled. Pure, so it is tested on every platform.
	SNK_API std::wstring quote_windows_arg(std::wstring_view arg);

	struct process_options
	{
		// Empty: the working directory of this process.
		std::filesystem::path working_dir;
		// The child's whole environment ("NAME=value" each) when set; the parent's environment otherwise.
		std::optional<std::vector<std::string>> environment;
		// `on_line` gets the output bytes in the chunks they were read in, not split into lines: for output that is
		// not 8-bit text (cmd /u writes UTF-16LE, where a 0x0A byte is not always a line break).
		bool raw_output = false;
	};

	struct process_result
	{
		int exit_code = -1;
		// Stopped through the stop_token. Also true when the stop raced a child that had already finished; exit_code
		// is then still its real exit code.
		bool cancelled = false;
		// Could not start (or another system failure); exit_code is then -1.
		std::string error;

		explicit operator bool() const { return error.empty() && !cancelled && exit_code == 0; }
	};

	// Runs `program` with `args` (UTF-8, the program itself not among them) and waits for it. A program without a
	// directory is looked up only in the PATH of the environment the child gets (on Windows with the PATHEXT
	// extensions; never in the current directory). stdout and stderr go to one pipe and arrive line by line in
	// `on_line` (without the line break; "\r\n", "\n" and a lone "\r" end a line; a line is cut at k_max_line_size; a
	// last line without a break is delivered too), on the calling thread. stdin is empty. A stop request ends the
	// process and everything it started, then returns with `cancelled`; what it started and left running is ended as
	// well once it exits. Blocks: call it from a worker thread.
	// Output bytes are passed through as received: a localized MSVC prints in the OEM code page (set VSLANG=1033 or
	// use /utf-8).
	// On POSIX the tree is the child's process group: descendants that start their own group or session escape it.
	SNK_API process_result run_process(const std::filesystem::path& program, const std::vector<std::string>& args,
		const process_options& options, const std::function<void(std::string_view line)>& on_line,
		std::stop_token stop);

	namespace detail
	{
		inline constexpr std::size_t k_max_line_size = 64 * 1024;

		// Appends `chunk` to `pending`, hands every complete line in it to `on_line` and keeps the rest; at the end of
		// the output the rest too, when there is any. Pure, so it is tested on every platform.
		SNK_API void take_lines(std::string& pending, std::string_view chunk, bool end_of_output,
			const std::function<void(std::string_view line)>& on_line);
		// take_lines, or with `raw` every non-empty chunk as it is (process_options::raw_output). Pure.
		SNK_API void take_output(bool raw, std::string& pending, std::string_view chunk, bool end_of_output,
			const std::function<void(std::string_view line)>& on_line);
	}
}
