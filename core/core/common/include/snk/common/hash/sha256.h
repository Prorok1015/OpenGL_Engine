#pragma once
#include <snk/common/snk_api.h>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>

// SHA-256 (FIPS 180-4): what the editor remembers of a project library the person trusted (#921).
namespace cmn
{
	// SHA-256 of a byte span, as 64 lowercase hex digits.
	SNK_API std::string sha256_hex(std::span<const std::byte> bytes);
	// SHA-256 of a file's contents read in chunks; nullopt when it cannot be read.
	SNK_API std::optional<std::string> sha256_file_hex(const std::filesystem::path& file);
	// SHA-256 of what `read` hands, in order, to the sink it is given; nullopt when `read` answers false (the content
	// could not be read). For a file read through a handle already held (#1304).
	SNK_API std::optional<std::string> sha256_stream_hex(
		const std::function<bool(const std::function<void(std::span<const std::byte>)>& sink)>& read);
}
