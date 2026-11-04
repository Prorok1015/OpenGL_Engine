#pragma once
#include <cstdint>

// The folder rule of platform::beneath::folder_entry_of (<snk/platform/plt_reparse_tag.h>, #523), copied: the MCP
// module includes nothing of the project (tools/ci/check_mcp_isolation.sh). Keep both in step; the test
// McpReparseRule_MatchesThePlatformRule compares them on a table of tags.
namespace mcp::detail
{
	inline constexpr std::uint32_t k_tag_mount_point = 0xA0000003u;
	inline constexpr std::uint32_t k_tag_symlink = 0xA000000Cu;
	inline constexpr std::uint32_t k_tag_cloud = 0x9000001Au;
	inline constexpr std::uint32_t k_tag_cloud_mask = 0x0000F000u;
	inline constexpr std::uint32_t k_tag_dedup = 0x80000013u;
	inline constexpr std::uint32_t k_attribute_directory = 0x10u;
	inline constexpr std::uint32_t k_attribute_reparse_point = 0x400u;

	constexpr bool tag_redirects(std::uint32_t tag)
	{
		return (tag & 0x20000000u) != 0 || tag == k_tag_mount_point || tag == k_tag_symlink;
	}

	enum class folder_entry : std::uint8_t { enter, through_filter, not_a_folder, refused };

	// A folder opened as itself: a plain folder is entered; one OneDrive/Cloud Files (CLOUD..CLOUD_F) or
	// deduplication keeps in place is entered through its filter, if it is still the same folder there; a name
	// surrogate (link, junction), any other tag or an unreadable one is refused.
	constexpr folder_entry folder_entry_of(std::uint32_t attributes, bool tag_known, std::uint32_t tag)
	{
		if ((attributes & k_attribute_directory) == 0) {
			return folder_entry::not_a_folder;
		}
		if ((attributes & k_attribute_reparse_point) == 0) {
			return folder_entry::enter;
		}
		const bool redirects = tag_redirects(tag);
		const bool kept_in_place = (tag & ~k_tag_cloud_mask) == k_tag_cloud || tag == k_tag_dedup;
		if (!tag_known || redirects || !kept_in_place) {
			return folder_entry::refused;
		}
		return folder_entry::through_filter;
	}

	enum class file_entry : std::uint8_t { plain, through_filter, refused };

	// A file opened as itself, as platform::beneath::read_beneath judges one: a folder, a name surrogate (link) or an
	// unreadable tag is refused; any other reparse point is a filter's file kept in place (a OneDrive placeholder, a
	// deduplicated file), read through the filter if it is still the same file there (#529).
	constexpr file_entry file_entry_of(std::uint32_t attributes, bool tag_known, std::uint32_t tag)
	{
		if ((attributes & k_attribute_directory) != 0) {
			return file_entry::refused;
		}
		if ((attributes & k_attribute_reparse_point) == 0) {
			return file_entry::plain;
		}
		if (!tag_known || tag_redirects(tag)) {
			return file_entry::refused;
		}
		return file_entry::through_filter;
	}
}
