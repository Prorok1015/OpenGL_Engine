#pragma once
#include <cstdint>

// Base only (include_base/, not installed in the SDK). Which Windows reparse points a read or a write below a root
// refuses (#484, #523): one that redirects a name (a symlink, a junction or any other name surrogate) could lead out of
// the root, while one a known file system filter keeps in place (OneDrive/Cloud Files placeholders, deduplicated files)
// is the file itself and is used through its filter; any other tag is refused. Pure, so it is tested on every platform.
namespace platform::beneath {

	inline constexpr std::uint32_t k_reparse_tag_mount_point = 0xA0000003u;
	inline constexpr std::uint32_t k_reparse_tag_symlink = 0xA000000Cu;

	// IsReparseTagNameSurrogate (winnt.h): bit 29 marks a tag that names another entry.
	constexpr bool reparse_tag_redirects(std::uint32_t tag)
	{
		return (tag & 0x20000000u) != 0 || tag == k_reparse_tag_mount_point || tag == k_reparse_tag_symlink;
	}

	inline constexpr std::uint32_t k_reparse_tag_cloud = 0x9000001Au;
	inline constexpr std::uint32_t k_reparse_tag_cloud_mask = 0x0000F000u;
	inline constexpr std::uint32_t k_reparse_tag_dedup = 0x80000013u;

	// The filters a folder may be entered through: IO_REPARSE_TAG_CLOUD and its variants CLOUD_1..CLOUD_F (the bits of
	// IO_REPARSE_TAG_CLOUD_MASK), and IO_REPARSE_TAG_DEDUP.
	constexpr bool reparse_tag_kept_in_place(std::uint32_t tag)
	{
		return (tag & ~k_reparse_tag_cloud_mask) == k_reparse_tag_cloud || tag == k_reparse_tag_dedup;
	}

	inline constexpr std::uint32_t k_file_attribute_directory = 0x10u; // FILE_ATTRIBUTE_DIRECTORY
	inline constexpr std::uint32_t k_file_attribute_reparse_point = 0x400u; // FILE_ATTRIBUTE_REPARSE_POINT

	enum class folder_entry : std::uint8_t { enter, through_filter, not_a_folder, refused };

	// What the Windows walk below a root does with a folder it opened as itself, reads (#484) and writes (#523) alike:
	// `attributes` are its FILE_ATTRIBUTE_* bits, `tag` its reparse tag (`tag_known` false: it could not be read). A
	// plain folder is entered, one a filter keeps in place (a OneDrive placeholder) is entered through the filter (if it
	// is still the same folder there), a link, a junction or any other tag is refused. Pure, so it is tested everywhere.
	constexpr folder_entry folder_entry_of(std::uint32_t attributes, bool tag_known, std::uint32_t tag)
	{
		if ((attributes & k_file_attribute_directory) == 0) {
			return folder_entry::not_a_folder;
		}
		if ((attributes & k_file_attribute_reparse_point) == 0) {
			return folder_entry::enter;
		}
		if (!tag_known || reparse_tag_redirects(tag) || !reparse_tag_kept_in_place(tag)) {
			return folder_entry::refused;
		}
		return folder_entry::through_filter;
	}

	enum class tree_entry : std::uint8_t { remove_itself, walk, walk_through_filter, refused };

	// What the Windows tree removal (#1294) does with an entry it opened as itself, decided as folder_entry_of decides
	// a folder of the walk: a plain folder is walked, one a filter keeps in place is walked through the filter, a file
	// is removed. A link, a junction or any other tag is never entered: at the top of the tree (`top`) it is refused,
	// inside it is removed as itself.
	constexpr tree_entry tree_entry_of(std::uint32_t attributes, bool tag_known, std::uint32_t tag, bool top)
	{
		const bool reparse_point = (attributes & k_file_attribute_reparse_point) != 0;
		if ((attributes & k_file_attribute_directory) == 0) {
			const bool kept_in_place = !reparse_point || (tag_known && reparse_tag_kept_in_place(tag));
			return top && !kept_in_place ? tree_entry::refused : tree_entry::remove_itself;
		}
		switch (folder_entry_of(attributes, tag_known, tag)) {
		case folder_entry::enter:
			return tree_entry::walk;
		case folder_entry::through_filter:
			return tree_entry::walk_through_filter;
		case folder_entry::not_a_folder:
		case folder_entry::refused:
			break;
		}
		return top ? tree_entry::refused : tree_entry::remove_itself;
	}

	inline constexpr std::uint32_t k_reparse_tag_wof = 0x80000017u;
	inline constexpr std::uint32_t k_reparse_tag_app_exec_link = 0x8000001Bu;

	enum class link_entry : std::uint8_t { plain, kept_in_place, link };

	// What the check of a project's folders when it is opened (#1295) makes of an entry opened as itself: a folder as
	// folder_entry_of decides it (any tag but a filter's that keeps it in place is a link); a file is a link when its
	// tag names another entry, is an app execution alias or cannot be read, kept in place for a filter's tag or a file
	// Windows compressed (WOF), and a link for any other tag. Pure, so it is tested everywhere.
	constexpr link_entry link_entry_of(std::uint32_t attributes, bool tag_known, std::uint32_t tag)
	{
		if ((attributes & k_file_attribute_directory) != 0) {
			switch (folder_entry_of(attributes, tag_known, tag)) {
			case folder_entry::enter:
				return link_entry::plain;
			case folder_entry::through_filter:
				return link_entry::kept_in_place;
			case folder_entry::not_a_folder:
			case folder_entry::refused:
				break;
			}
			return link_entry::link;
		}
		if ((attributes & k_file_attribute_reparse_point) == 0) {
			return link_entry::plain;
		}
		if (!tag_known || reparse_tag_redirects(tag) || tag == k_reparse_tag_app_exec_link) {
			return link_entry::link;
		}
		return reparse_tag_kept_in_place(tag) || tag == k_reparse_tag_wof ? link_entry::kept_in_place : link_entry::link;
	}

}
