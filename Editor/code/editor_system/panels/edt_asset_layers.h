#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace edt
{
	struct layer_entry {
		std::filesystem::path path;     // physical path in the layer that wins for this entry
		std::filesystem::path relative; // path under res:// (the layer-independent identity)
		std::string name;
		bool is_dir = false;
		size_t layer = 0;               // index into the layers list
	};

	// Key under which names from different layers are the same entry: the file system folds case on Windows.
	std::string layer_merge_key(const std::string& name, bool case_insensitive);

	// Merged, GUI-free view of one directory across resource layers. layers is ordered like the resource
	// system mounts them: lowest first, top last. An entry of an upper layer shadows the same name below
	// (whatever its kind); directories first, then files, each sorted by name. A layer lacking the directory is skipped.
	// dirs_only skips files (the directory tree does not need them).
	std::vector<layer_entry> list_merged_directory(
		const std::vector<std::filesystem::path>& layers,
		const std::filesystem::path& relative_dir,
		bool dirs_only = false);
}
