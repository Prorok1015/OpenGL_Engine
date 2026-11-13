#include <snk/resource/res_asset_walk.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/platform/plt_beneath.h>
#include <cstddef>
#include <map>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <snk/platform/plt_reparse_tag.h>
#endif

namespace fs = std::filesystem;

namespace
{
	struct walk_state
	{
		const std::vector<fs::path>& layers;
		const std::function<bool()>& stopped;
		std::vector<res::asset_walk_file>& out;
		bool case_insensitive = false;
		bool depth_warned = false;
	};

	// One folder of the merged tree. `in_layers`: the layers (ascending) where `relative_dir` is a real folder.
	void walk(walk_state& state, const fs::path& relative_dir, const std::vector<std::size_t>& in_layers, int depth)
	{
		if (depth >= res::asset_walk_max_depth) {
			if (!state.depth_warned) {
				egLOG_WARN("assets", "res://{}: deeper than {} folders, not walked further", fs_paths::generic_utf8_of(relative_dir),
					res::asset_walk_max_depth);
				state.depth_warned = true;
			}
			return;
		}
		struct slot
		{
			fs::path relative;
			fs::path physical;
			bool is_dir = false;
			std::size_t layer = 0;
			std::vector<std::size_t> dir_layers;
			std::vector<std::size_t> shadowed;
		};
		std::map<std::string, slot> merged;
		for (const std::size_t layer : in_layers) {
			std::error_code ec;
			fs::directory_iterator it(state.layers[layer] / relative_dir, fs::directory_options::skip_permission_denied, ec);
			for (; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
				if (state.stopped && state.stopped()) return;
				const fs::directory_entry& entry = *it;
				if (res::asset_entry_redirects(entry)) continue;
				std::error_code kind_ec;
				const bool is_dir = entry.is_directory(kind_ec);
				if (kind_ec) continue;
				const std::string name = fs_paths::utf8_of(entry.path().filename());
				if (res::is_skipped_asset_name(name, is_dir)) continue;
				slot& found = merged[res::asset_name_key(name, state.case_insensitive)];
				const bool existed = !found.relative.empty();
				if (existed) found.shadowed.push_back(found.layer);
				if (is_dir && existed && found.is_dir) {
					found.dir_layers.push_back(layer);
				} else if (is_dir) {
					found.dir_layers = { layer };
				} else {
					found.dir_layers.clear();
				}
				found.relative = relative_dir / entry.path().filename();
				found.physical = entry.path();
				found.is_dir = is_dir;
				found.layer = layer;
			}
		}
		for (auto& [key, entry] : merged) {
			if (state.stopped && state.stopped()) return;
			if (entry.is_dir) {
				walk(state, entry.relative, entry.dir_layers, depth + 1);
			} else {
				state.out.push_back(res::asset_walk_file{ std::move(entry.physical), std::move(entry.relative), entry.layer, std::move(entry.shadowed) });
			}
		}
	}
}

std::vector<res::asset_walk_file> res::walk_asset_layers(const std::vector<fs::path>& layers, const std::function<bool()>& stopped,
	bool case_insensitive)
{
	std::vector<asset_walk_file> files;
	std::vector<std::size_t> all_layers(layers.size());
	for (std::size_t i = 0; i < layers.size(); ++i) all_layers[i] = i;
	walk_state state{ layers, stopped, files, case_insensitive, false };
	walk(state, fs::path{}, all_layers, 0);
	return files;
}

res::asset_walk_pairing res::pair_asset_metas(const std::vector<asset_walk_file>& files, bool case_insensitive)
{
	std::unordered_map<std::string, std::size_t> metas;
	std::vector<std::size_t> sidecars;
	for (std::size_t i = 0; i < files.size(); ++i) {
		const std::string relative = fs_paths::generic_utf8_of(files[i].relative);
		if (!is_asset_meta_sidecar(relative)) continue;
		sidecars.push_back(i);
		// A second sidecar of one owner ("a.png.resource" beside "a.png.RESOURCE" where letter case counts) pairs with
		// nothing: it is an orphan.
		metas.emplace(asset_name_key(asset_meta_owner(relative), case_insensitive), i);
	}
	asset_walk_pairing out;
	out.meta_of.resize(files.size());
	std::vector<bool> paired(files.size(), false);
	for (std::size_t i = 0; i < files.size(); ++i) {
		const std::string relative = fs_paths::generic_utf8_of(files[i].relative);
		if (is_asset_meta_sidecar(relative)) continue;
		if (const auto meta = metas.find(asset_name_key(relative, case_insensitive)); meta != metas.end()) {
			out.meta_of[i] = meta->second;
			paired[meta->second] = true;
		}
	}
	for (const std::size_t index : sidecars) {
		if (!paired[index]) out.orphans.push_back(index);
	}
	return out;
}

bool res::asset_entry_redirects(const fs::directory_entry& entry)
{
	std::error_code ec;
	if (entry.is_symlink(ec) || ec) return true;
#if defined(_WIN32)
	WIN32_FIND_DATAW data{};
	// The \\?\ prefix lifts MAX_PATH for an absolute path, and turns off the normalization that would make a '/' a
	// separator: the path goes with '\' only.
	std::wstring query = fs::path(entry.path()).make_preferred().native();
	if (entry.path().is_absolute() && !query.starts_with(LR"(\\?\)")) query.insert(0, LR"(\\?\)");
	const HANDLE found = ::FindFirstFileW(query.c_str(), &data);
	if (found == INVALID_HANDLE_VALUE) {
		egLOG_WARN("assets", "{}: cannot be inspected ({}), left out of the asset walk", fs_paths::utf8_of(entry.path()),
			std::system_category().message(static_cast<int>(::GetLastError())));
		return true;
	}
	::FindClose(found);
	if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return false;
	return platform::beneath::reparse_tag_redirects(data.dwReserved0) || !platform::beneath::reparse_tag_kept_in_place(data.dwReserved0);
#else
	return false;
#endif
}

res::asset_meta_file res::read_asset_meta_file(const fs::path& layer_root, const fs::path& relative)
{
	asset_meta_file out;
	platform::beneath::read_request request;
	request.root = layer_root;
	request.folder = relative.parent_path();
	request.leaf = relative.filename();
	request.max_bytes = asset_meta_max_bytes;
	std::vector<std::byte> bytes;
	const platform::beneath::read_result result = platform::beneath::read_beneath(request, bytes);
	if (result.status == platform::beneath::read_status::missing) return out;
	out.present = true;
	if (result.status != platform::beneath::read_status::ok) {
		out.error = result.error.empty() ? std::string{ "cannot be read" } : result.error;
		return out;
	}
	const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	asset_meta_result read = read_asset_meta(text);
	if (!read) {
		out.error = read.error.field_path.empty() ? read.error.message : read.error.field_path + ": " + read.error.message;
		return out;
	}
	out.meta = std::move(*read.meta);
	return out;
}
