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

	// Merged, GUI-free view of one directory across resource layers. layers is ordered like the resource
	// system mounts them: lowest first, top last. An entry of an upper layer shadows the same name below
	// (whatever its kind); directories first, then files, each sorted by name. A layer lacking the directory is skipped.
	std::vector<layer_entry> list_merged_directory(
		const std::vector<std::filesystem::path>& layers,
		const std::filesystem::path& relative_dir);
}
