#pragma once
#include <filesystem>
#include <string>
#include <snk/common/snk_api.h>

namespace platform
{
	// Opens a file with the program the system associates with it (an IDE or text editor for source code) and does not
	// wait for it. `file` must be an absolute path to an existing regular file. The program is started with argument
	// vectors only, never through a shell command line (Windows: ShellExecuteExW with the wide path, no UI of its own;
	// macOS: /usr/bin/open; elsewhere: xdg-open found in PATH).
	// Empty on success, else why not: also when nothing is associated with the file (Windows, where the system tells
	// at once). On Linux xdg-open reports a missing handler only after it started, so that case is not seen here.
	SNK_API std::string open_in_shell(const std::filesystem::path& file);
}
