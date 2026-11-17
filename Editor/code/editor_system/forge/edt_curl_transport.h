#pragma once
#include "edt_http_transport.h"
#include "edt_toolchain.h"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace edt::forge
{
	struct curl_options
	{
		// Empty: `curl` (`curl.exe` on Windows) on the PATH of the editor's environment.
		std::filesystem::path curl;
		// Hosts a request may go to (lowercase compare); anything else is refused before curl runs.
		std::vector<std::string> allowed_hosts;
		int max_time_seconds = 30;
		std::size_t max_response_bytes = 1024 * 1024;
		process_runner run = platform::run_process;
		// How long past max_time_seconds a curl that does not end is waited for before it is stopped.
		std::chrono::seconds deadline_grace{ 5 };
		// Where the request's files go; empty path: it cannot be made. Null: a new owner-only folder per request
		// (mkdtemp, 0700, in $XDG_RUNTIME_DIR when that is usable, else the OS temp folder; on Windows a folder with a
		// protected DACL for the current user), removed when the request ends. A folder given here is the caller's
		// business: on POSIX it is only checked to be a real directory (not a link), and only the request's files are
		// removed from it.
		std::function<std::filesystem::path()> scratch_dir;
		// The environment curl runs in; nullopt: this process's, which keeps the system proxy settings. CURL_HOME and
		// SSLKEYLOGFILE are dropped either way (the first points curl at another config, the second makes it write the
		// TLS secrets to a file).
		std::optional<std::vector<std::string>> environment;
	};

	// cfg forge/curl (the curl program) and editor/forge_hosts (default ["api.github.com"]).
	curl_options configured_curl_options();

	// HTTPS through a `curl` subprocess. The bearer token reaches curl only through a `-K` config file (0600 on POSIX,
	// a protected owner-only DACL on Windows) in a per-request private folder (never argv, never the environment, never
	// a log), deleted (overwritten first) when curl ends, whatever the outcome. GET and POST only. A curl that outlives
	// max_time_seconds + deadline_grace is stopped. Only https to an allowed host without userinfo or port; redirects are never
	// followed (a 3xx is an error); ~/.curlrc is ignored (`-q`).
	http_transport make_curl_transport(curl_options options);
}
