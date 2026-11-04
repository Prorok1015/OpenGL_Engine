#pragma once
#include "edt_http_transport.h"
#include "edt_toolchain.h"
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
		// A private (0700) folder for the request's files; empty path: it cannot be made. Null: one under the OS temp
		// folder, made on the first request and removed with the transport.
		std::function<std::filesystem::path()> scratch_dir;
		// The environment curl runs in; nullopt: this process's, which keeps the system proxy settings. CURL_HOME is
		// dropped either way (it would let the environment point curl at another config).
		std::optional<std::vector<std::string>> environment;
	};

	// cfg forge/curl (the curl program) and editor/forge_hosts (default ["api.github.com"]).
	curl_options configured_curl_options();

	// HTTPS through a `curl` subprocess. The bearer token reaches curl only through a 0600 `-K` config file in the
	// private folder (never argv, never the environment, never a log), deleted (the token's file overwritten first)
	// when curl ends, whatever the outcome. Only https to an allowed host without userinfo or port; redirects are never
	// followed (a 3xx is an error); ~/.curlrc is ignored (`-q`).
	http_transport make_curl_transport(curl_options options);
}
