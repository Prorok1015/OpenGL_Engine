#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace mcp
{
	// 256 random bits from the operating system, as 64 lower-case hex characters. A bearer token.
	std::string generate_token();

	struct connection_info
	{
		std::uint16_t port = 0;
		std::string token;

		// "http://127.0.0.1:<port>/mcp"
		std::string url() const;
	};

	// Writes <directory>/mcp.json, {"url": ..., "port": ..., "token": ...}, creating the directory. On POSIX the file is
	// created readable by the current user only (0600) before the token goes in. On Windows it gets the permissions
	// of the directory (the user's profile or project folder); there is no portable way to narrow them from here.
	// Returns the reason when it could not be written, an empty string on success.
	std::string write_connection_file(const std::filesystem::path& directory, const connection_info& info);

	// Removes <directory>/mcp.json; a missing file is fine.
	void remove_connection_file(const std::filesystem::path& directory);

	// <directory>/mcp.json
	std::filesystem::path connection_file_path(const std::filesystem::path& directory);
}
