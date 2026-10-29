#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mcp
{
	struct server_config
	{
		// Bearer token every request must carry. Empty: server::start() makes a random one.
		std::string token;
		// Loopback port. 0: the system picks one (server::port() tells which).
		std::uint16_t port = 0;
		// The one URL path served ("/mcp").
		std::string path = "/mcp";
		// Where start() writes {"url","port","token"} for clients, readable by the current user only. Empty: no file.
		// The file is removed again by stop().
		std::filesystem::path connection_file;

		std::string server_name = "snk-editor";
		std::string server_version = "0";
		// Version of the command set; reported as serverInfo.snk_api. Empty: left out.
		std::string api_version;
		// Text for initialize.instructions. Empty: left out.
		std::string instructions;

		// Origins accepted besides http(s)://127.0.0.1, localhost and [::1] (any port). A request without Origin
		// (every non-browser client) is fine.
		std::vector<std::string> allowed_origins;

		// Largest accepted request body.
		std::size_t max_message_bytes = 1024 * 1024;
		std::size_t max_sessions = 16;
		std::size_t max_connections = 32;
		// Tool calls and resource reads that may be waiting for the host at once.
		std::size_t max_in_flight = 8;
		// Per read of a request head or body.
		std::chrono::seconds io_timeout{ 30 };
	};
}
