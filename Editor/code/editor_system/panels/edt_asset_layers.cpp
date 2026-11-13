#include "edt_asset_layers.h"
#include <snk/common/fs_paths.h>
#include <snk/resource/res_asset_walk.h>
#include <algorithm>
#include <map>

namespace fs = std::filesystem;

std::string edt::layer_merge_key(const std::string& name, bool case_insensitive)
{
	return res::asset_name_key(name, case_insensitive);
}

std::vector<edt::layer_entry> edt::list_merged_directory(
	const std::vector<fs::path>& layers,
	const fs::path& relative_dir,
	bool dirs_only)
{
	constexpr bool case_insensitive = res::asset_names_case_insensitive;
	std::map<std::string, layer_entry> merged;
	for (size_t i = 0; i < layers.size(); ++i) {
		const fs::path dir = layers[i] / relative_dir;
		std::error_code ec;
		if (!fs::is_directory(dir, ec) || ec)
			continue;

		for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator{}; it.increment(ec)) {
			std::error_code ec2;
			const bool is_dir = it->is_directory(ec2);
			if (ec2 || (dirs_only && !is_dir)) continue;

			layer_entry e;
			e.path = it->path();
			e.name = fs_paths::utf8_of(e.path.filename());
			e.relative = relative_dir / e.path.filename();
			e.is_dir = is_dir;
			e.layer = i;
			merged[layer_merge_key(e.name, case_insensitive)] = std::move(e);
		}
	}

	std::vector<layer_entry> out;
	out.reserve(merged.size());
	for (auto& kv : merged)
		out.push_back(std::move(kv.second));
	std::stable_sort(out.begin(), out.end(), [](const layer_entry& a, const layer_entry& b) {
		return a.is_dir && !b.is_dir;
	});
	return out;
}
