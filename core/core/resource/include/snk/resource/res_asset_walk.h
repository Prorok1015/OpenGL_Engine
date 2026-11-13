#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>
#include <snk/resource/res_asset_meta.h>

// The rules every walk of a resource tree for its assets follows (EPIC-57 4.3, EPIC-58 S5 #756): the editor's asset
// database and the project migration's index both take them from here, so they never list different files.
namespace res
{
	// A walk lists nothing in a folder this many names deep or deeper.
	inline constexpr int asset_walk_max_depth = 64;
	// A larger .resource is not read.
	inline constexpr std::uint64_t asset_meta_max_bytes = 64u * 1024u;

	// Names of one folder are the same file in any letter case (Windows); a walk merges and pairs them by asset_name_key.
#if defined(_WIN32)
	inline constexpr bool asset_names_case_insensitive = true;
#else
	inline constexpr bool asset_names_case_insensitive = false;
#endif

	// The key two names (or paths) share when a walk takes them for the same file: ASCII letters folded when
	// `case_insensitive`.
	inline std::string asset_name_key(std::string_view name, bool case_insensitive)
	{
		std::string key{ name };
		if (case_insensitive) {
			for (char& ch : key) {
				if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
			}
		}
		return key;
	}

	// A .resource sidecar (any letter case) of some file: a name before the suffix. `path` may be a name or a path with '/'.
	inline bool is_asset_meta_sidecar(std::string_view path)
	{
		const std::string_view name = path.substr(path.find_last_of('/') + 1);
		return name.size() > asset_meta_suffix.size() && is_asset_meta_name(name);
	}

	// The path of the file the sidecar `path` belongs to.
	inline std::string_view asset_meta_owner(std::string_view path)
	{
		return path.substr(0, path.size() - asset_meta_suffix.size());
	}

	// A name a walk neither lists nor enters: one is_ignored_asset_name rejects (a hidden folder, a dot file, a
	// temporary or system file), or the .resource of such a file.
	inline bool is_skipped_asset_name(std::string_view name, bool is_dir)
	{
		if (is_ignored_asset_name(name)) return true;
		return !is_dir && is_asset_meta_sidecar(name) && is_ignored_asset_name(asset_meta_owner(name));
	}

	// The project file: a layer that holds it in its root is the project folder itself (a content root ".").
	inline constexpr std::string_view project_file_name = "project.snkproj";

	// A name directly in the project folder that is no content even when the folder is a content root ".": the project's
	// code, its build output and project.snkproj.
	inline bool is_project_own_name(std::string_view name)
	{
		return name == "code" || name == "build" || name == project_file_name;
	}

	// The layer's root holds project_file_name (a regular file, not a link).
	SNK_API bool is_project_folder_layer(const std::filesystem::path& layer);

	// What a walk neither lists nor enters: is_skipped_asset_name, and in the root of a layer that is the project folder
	// (`at_project_folder_root`) the project's own names and their .resource. A skipped name shadows nothing below.
	inline bool is_skipped_asset_entry(std::string_view name, bool is_dir, bool at_project_folder_root)
	{
		if (is_skipped_asset_name(name, is_dir)) return true;
		if (!at_project_folder_root) return false;
		return is_project_own_name(name) || (!is_dir && is_asset_meta_sidecar(name) && is_project_own_name(asset_meta_owner(name)));
	}

	// An entry a walk neither lists nor enters: a link, or on Windows a junction or any reparse point other than one a
	// file system filter keeps in place (a OneDrive placeholder is the file itself). An entry that cannot be inspected
	// redirects too (a warning names it).
	SNK_API bool asset_entry_redirects(const std::filesystem::directory_entry& entry);

	// One file of the merged tree of `layers` (lowest first): the entry of the highest layer wins a name.
	struct asset_walk_file
	{
		std::filesystem::path physical;
		// Relative to every layer, with the letter case of the winning entry.
		std::filesystem::path relative;
		std::size_t layer = 0;
		// Lower layers that hold an entry of the same name, shadowed by this one.
		std::vector<std::size_t> shadowed;
	};

	// Every file of the layers merged by asset_name_key, .resource files included, by the rules above: links and
	// redirecting reparse points and skipped entries (is_skipped_asset_entry) are neither listed nor entered, nothing deeper than
	// asset_walk_max_depth (one warning). Stops (returning what it has) as soon as `stopped` (may be empty) says so.
	SNK_API std::vector<asset_walk_file> walk_asset_layers(const std::vector<std::filesystem::path>& layers,
		const std::function<bool()>& stopped, bool case_insensitive);

	// Which .resource of a walk belongs to which file: a sidecar belongs to the file whose path has its owner's
	// asset_name_key ("a.png.RESOURCE" to "a.png" everywhere, "A.png.resource" too when case insensitive).
	struct asset_walk_pairing
	{
		// Parallel to the walk: for a file that is no .resource, the index of its .resource, if any.
		std::vector<std::optional<std::size_t>> meta_of;
		// Indices of every .resource no file pairs with, in the order of the walk.
		std::vector<std::size_t> orphans;
	};

	SNK_API asset_walk_pairing pair_asset_metas(const std::vector<asset_walk_file>& files, bool case_insensitive);

	struct asset_meta_file
	{
		// A file is there (readable or not).
		bool present = false;
		std::optional<asset_meta> meta;
		// Why a present file gave no meta.
		std::string error;
	};

	// Reads <layer_root>/<relative> below the root without following a link, at most asset_meta_max_bytes.
	SNK_API asset_meta_file read_asset_meta_file(const std::filesystem::path& layer_root, const std::filesystem::path& relative);
}
