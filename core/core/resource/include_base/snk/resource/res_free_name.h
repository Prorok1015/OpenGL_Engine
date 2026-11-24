#pragma once
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>

// Base only (include_base/, not installed in the SDK): whether the name of a new res:// file is free in every layer, so an
// automatically made file never shadows one below it or inherits a leftover file's ID (#1279). An override of an engine
// file is still legal; it is just never made by these.
//
// Advisory only: a name may be taken between the check and the write. Creation itself stays exclusive (create_sync, a
// write with replace = false), which is what catches that race: never drop it because the name was checked.
namespace res
{
	struct name_refusal
	{
		// Index into the layer list given (lowest layer first).
		std::size_t layer = 0;
		// What holds the name, or the folder that could not be read.
		std::filesystem::path existing;
		// The name is held by a leftover `.resource` (a deleted file's ID would pass to the new file).
		bool meta_left = false;
		// Why the folder could not be read; empty when something holds the name.
		std::string error;
	};

	// Whether `relative` (below res://, '/' separated) is taken in any of `layers` (lowest first), walked highest first.
	// Each folder is listed once without following a link; a missing folder (or a file on the way) is skipped, any other
	// failure is a refusal with `error`. Names compare with ASCII letters folded on every platform (asset_name_key), so a
	// project made on Linux still works on Windows; Unicode normalization is not folded. Taken by an entry of any kind
	// (file, folder, link) named `relative`, its `.resource`, and for `<x>.<ext>.desc` a leftover `<x>.<ext>.bin` or
	// `<x>.<ext>.bin.new` (res::desc_binary::binary_tag).
	SNK_API std::optional<name_refusal> name_taken(const std::vector<std::filesystem::path>& layers, std::string_view relative);

	struct free_name_result
	{
		// The file name (no folder); empty on error.
		std::string name;
		std::string error;

		bool ok() const { return error.empty(); }
	};

	// The first of `stem + suffix`, `stem + separator + "2" + suffix`, ... up to the number `max` that name_taken would not
	// refuse in `folder` and for which `also_taken` (given the path below res://, may be empty) is false; the number goes
	// before the whole suffix ("rock-2.shader.desc"). Each layer's folder is read once. An error for an unreadable folder,
	// for no free name up to `max`, and for a candidate that is no portable name (res::portable_path_problem, a name over
	// res::max_sanitized_part_length bytes): such a candidate is never skipped.
	SNK_API free_name_result free_name(const std::vector<std::filesystem::path>& layers, std::string_view folder,
		std::string_view stem, std::string_view suffix, std::string_view separator = "-",
		const std::function<bool(std::string_view)>& also_taken = {}, int max = 999);
}
