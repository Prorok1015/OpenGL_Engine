#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace edt
{
	// The person's choice about the agent server (the agents window, #449): edt://settings/agent_server.json, in the
	// person's own settings, never in the project and never written by a command. Without a file, or with one that cannot
	// be read, the server stays off.
	struct agent_server_settings
	{
		bool enabled = false;
		// 0: the operating system picks a free port.
		std::uint16_t port = 0;

		bool operator==(const agent_server_settings&) const = default;
	};

	// {"enabled": true, "port": 8123}; both keys optional, an unknown key, a wrong type or a port outside 0..65535 is an
	// error (the settings are then the defaults: off).
	struct agent_server_settings_result
	{
		agent_server_settings settings;
		std::string error;
	};
	agent_server_settings_result parse_agent_server_settings(std::string_view text);
	std::string serialize_agent_server_settings(const agent_server_settings& settings);

	// A missing file is the defaults without a word; an unreadable or bad one is the defaults with a warning.
	agent_server_settings load_agent_server_settings(const std::filesystem::path& file);
	// Atomically; the error, nullopt on success.
	std::optional<std::string> save_agent_server_settings(const std::filesystem::path& file, const agent_server_settings& settings);
}
