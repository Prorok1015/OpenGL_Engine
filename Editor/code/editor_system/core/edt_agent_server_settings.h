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

	// cfg `editor/agent_server` is a switch with three states: not set, true, false. An explicit false wins over the person's
	// own choice, so a run that must not open a port (CI, an agent's `--headless --mcp-stdio`) can say so.
	enum class cfg_switch : std::uint8_t { unset, on, off };
	// "" is unset, "true" and "1" are on, anything else (false, 0, a typo) is off: when in doubt no port.
	cfg_switch parse_cfg_switch(std::string_view text);

	struct agent_http_choice
	{
		bool enabled = false;
		std::uint16_t port = 0;
	};
	// Whether the HTTP transport starts with the editor. The cfg switch decides when it is set (its own port: `cfg_port`, 0 or
	// out of range = any). Otherwise the person's file decides, except headless or with the stdio transport (a driven
	// editor, not a person's session): there the file is ignored and the transport stays off.
	agent_http_choice decide_agent_http(cfg_switch cfg, int cfg_port, const agent_server_settings& person, bool headless, bool stdio);

	// A missing file is the defaults without a word; an unreadable or bad one is the defaults with a warning.
	agent_server_settings load_agent_server_settings(const std::filesystem::path& file);
	// Atomically; the error, nullopt on success.
	std::optional<std::string> save_agent_server_settings(const std::filesystem::path& file, const agent_server_settings& settings);
}
