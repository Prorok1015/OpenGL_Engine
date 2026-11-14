#pragma once
#include <snk/common/snk_api.h>
#include <cstddef>
#include <filesystem>
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
}
