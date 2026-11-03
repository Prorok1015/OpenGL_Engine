#pragma once
#include <cstdint>

// Base only (include_base/, not installed in the SDK). Which Windows reparse points a vfs read refuses (#484): one that
// redirects a name (a symlink, a junction or any other name surrogate) could lead out of the root, while one a file
// system filter keeps in place (OneDrive/Cloud Files placeholders, deduplicated or WCI files) is the file itself and is
// read through its filter. Pure, so it is tested on every platform.
namespace res::detail {

	inline constexpr std::uint32_t k_reparse_tag_mount_point = 0xA0000003u;
	inline constexpr std::uint32_t k_reparse_tag_symlink = 0xA000000Cu;

	// IsReparseTagNameSurrogate (winnt.h): bit 29 marks a tag that names another entry.
	constexpr bool reparse_tag_redirects(std::uint32_t tag)
	{
		return (tag & 0x20000000u) != 0 || tag == k_reparse_tag_mount_point || tag == k_reparse_tag_symlink;
	}

}
