#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace mcp
{
	// 256 random bits from the operating system, as 64 lower-case hex characters. A bearer token.
	std::string generate_token();

	// The id of this process, and whether a process with that id is running.
	std::int64_t current_process_id();
	bool process_is_running(std::int64_t pid);

	struct connection_info
	{
		std::uint16_t port = 0;
		std::string token;
		// The editor process that owns the file; two editors on one project must not take each other's file.
		std::int64_t pid = 0;

		// "http://127.0.0.1:<port>/mcp"
		std::string url() const;
	};

	// <directory>/mcp.json
	std::filesystem::path connection_file_path(const std::filesystem::path& directory);

	// The file as it is now; nullopt if it is missing or unreadable.
	std::optional<connection_info> read_connection_file(const std::filesystem::path& directory);

	// Writes <directory>/mcp.json, {"url", "port", "token", "pid"}, creating the directory.
	//  - The token is never readable by other users: on POSIX the file is created 0600 (the new directory 0700); on
	//    Windows it gets an explicit DACL that grants the current user alone, set when the file is created. If that
	//    cannot be done nothing is written.
	//  - The file is written under a temporary name (created exclusively, links are not followed) and renamed over the
	//    old one, so a link planted at mcp.json is replaced and never followed, and a reader never sees half a file.
	//    A <directory> that is a link (symlink, or a reparse point on Windows) is refused.
	//  - An existing file of another process that is still running is refused; one of a dead process, of this one, or an
	//    unreadable one is replaced.
	// Returns the reason when it could not be written, an empty string on success.
	std::string write_connection_file(const std::filesystem::path& directory, const connection_info& info);

	// Removes <directory>/mcp.json if it still holds the token and pid of `mine`; a file of another editor is left alone.
	void remove_connection_file(const std::filesystem::path& directory, const connection_info& mine);
}
