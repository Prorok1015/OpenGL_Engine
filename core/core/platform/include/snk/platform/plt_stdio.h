#pragma once
#include <string_view>
#include <snk/common/snk_api.h>

namespace platform
{
	// The descriptor a protocol on the process' stdout writes to (MCP over stdio, #452), once take_stdout_for_protocol
	// moved everything else to stderr. Main puts it in the app_data_storage.
	struct protocol_stdout
	{
		explicit protocol_stdout(int fd_) : fd(fd_) {}
		int fd = -1;
	};

	// Points `stdout_fd` at what `stderr_fd` is and returns a new descriptor of what `stdout_fd` was before, so only
	// the one who holds it can still write there. -1 on failure, and `stdout_fd` is left as it was.
	SNK_API int redirect_stdout(int stdout_fd, int stderr_fd);

	// The process' stdout for a protocol: flushes stdout, then redirect_stdout of the C runtime's stdout and stderr; on
	// Windows also STD_OUTPUT_HANDLE, and the returned descriptor is in binary mode (lines end in "\n" only). POSIX:
	// SIGPIPE is ignored, so a client that went away is a write error, not the end of the process. Call it before anything
	// else writes. -1 on failure.
	SNK_API int take_stdout_for_protocol();

	// Writes all of `data`, over partial writes and interruptions. False on an error.
	SNK_API bool write_all(int fd, std::string_view data);
}
