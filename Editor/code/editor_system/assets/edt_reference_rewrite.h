#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Rewriting the references to one file inside another file's text (EPIC-57 section 6.4, #775): the places come from the
// usages work list (a JSON pointer each), nothing is found by walking the file again.
namespace edt
{
	enum class rewrite_status : std::uint8_t
	{
		rewritten,
		// Every pointer already holds the new path (or is a {guid} without a path): the file stays as it is.
		nothing_to_do,
		// Not JSON, or nested deeper than the readers go.
		unreadable,
		// Parsing the text and writing it again does not give its bytes back (another writer's layout, a number the
		// writer spells otherwise): the file is not touched.
		not_canonical,
		// A pointer leads nowhere in the file: it changed since the usages were read.
		pointer_missing,
		// The value at a pointer is neither a string nor an object, or names another file.
		not_a_reference,
		// An object at a pointer has a guid that is not the target's: it refers to another asset (or to none), so its path is
		// left alone and the file is not rewritten.
		guid_mismatch,
	};

	std::string_view rewrite_status_name(rewrite_status status);

	struct rewrite_outcome
	{
		rewrite_status status = rewrite_status::nothing_to_do;
		std::string reason;
		// The whole file, for `rewritten` only.
		std::string text;
		// The pointers that changed, in the order given.
		std::vector<std::string> pointers;
	};

	// The text as the project writes its JSON files (res::pretty_json_bytes).
	std::string canonical_json_text(std::string_view text, std::string& error);

	// Replaces `old_path` by `new_path` at each of `pointers` in `text`: a string naming the old path becomes the new
	// one; an object keeps its other members (the guid) and gets the new "path" only when its guid is `target_guid` (any
	// other guid, or none known, fails the file as guid_mismatch: it is not this asset's reference), one without a guid must
	// name the old path; a {guid} with no path holds nothing to change.
	// Paths are compared as res::asset_name_key does. The text is edited only when parse then write gives it back unchanged.
	rewrite_outcome rewrite_references(std::string_view text, std::span<const std::string> pointers, std::string_view old_path,
		std::string_view new_path, bool case_insensitive, std::string_view target_guid = {});
}
