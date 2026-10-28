#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>

namespace platform
{
	// The file of the running program; empty when the system cannot tell.
	SNK_API std::filesystem::path current_executable();

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
}
