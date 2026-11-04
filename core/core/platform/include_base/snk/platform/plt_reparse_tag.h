#pragma once
#include <cstdint>

// Base only (include_base/, not installed in the SDK). Which Windows reparse points a read or a write below a root
// refuses (#484, #523): one that redirects a name (a symlink, a junction or any other name surrogate) could lead out of
// the root, while one a file system filter keeps in place (OneDrive/Cloud Files placeholders, deduplicated or WCI files)
// is the file itself and is used through its filter. Pure, so it is tested on every platform.
namespace platform::beneath {

	inline constexpr std::uint32_t k_reparse_tag_mount_point = 0xA0000003u;
	inline constexpr std::uint32_t k_reparse_tag_symlink = 0xA000000Cu;

	// IsReparseTagNameSurrogate (winnt.h): bit 29 marks a tag that names another entry.
	constexpr bool reparse_tag_redirects(std::uint32_t tag)
	{
		return (tag & 0x20000000u) != 0 || tag == k_reparse_tag_mount_point || tag == k_reparse_tag_symlink;
	}

	inline constexpr std::uint32_t k_file_attribute_directory = 0x10u; // FILE_ATTRIBUTE_DIRECTORY
	inline constexpr std::uint32_t k_file_attribute_reparse_point = 0x400u; // FILE_ATTRIBUTE_REPARSE_POINT

	enum class folder_entry : std::uint8_t { enter, through_filter, not_a_folder, refused };

	// What the Windows walk below a root does with a folder it opened as itself, reads (#484) and writes (#523) alike:
	// `attributes` are its FILE_ATTRIBUTE_* bits, `tag` its reparse tag (`tag_known` false: it could not be read). A
	// plain folder is entered, one a filter keeps in place (a OneDrive placeholder) is entered through the filter (if it
	// is still the same folder there), a link, a junction or an unknown tag is refused. Pure, so it is tested everywhere.
	constexpr folder_entry folder_entry_of(std::uint32_t attributes, bool tag_known, std::uint32_t tag)
	{
		if ((attributes & k_file_attribute_directory) == 0) {
			return folder_entry::not_a_folder;
		}
		if ((attributes & k_file_attribute_reparse_point) == 0) {
			return folder_entry::enter;
		}
		if (!tag_known || reparse_tag_redirects(tag)) {
			return folder_entry::refused;
		}
		return folder_entry::through_filter;
	}

}
