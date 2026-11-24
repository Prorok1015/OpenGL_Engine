#include <snk/resource/res_free_name.h>
#include <format>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <snk/common/fs_paths.h>
#include <snk/platform/plt_beneath.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/res_path_sanitize.h>

namespace
{
	namespace fs = std::filesystem;

	// A folder with more entries is not read whole, so no name in it can be called free.
	constexpr std::size_t k_max_folder_entries = std::size_t{ 1 } << 20;

	std::string key_of(std::string_view name)
	{
		return res::asset_name_key(name, true);
	}

	struct layer_folder
	{
		bool missing = false;
		std::string error;
		// Folded key -> the entry's name as the folder spells it.
		std::unordered_map<std::string, std::string> names;
	};

	// A file (not a link) somewhere on the way to `folder`: the folder cannot exist in this layer, as for a missing one.
	bool blocked_by_file(const fs::path& layer, const fs::path& folder)
	{
		fs::path at = layer;
		for (const fs::path& part : folder) {
			at /= part;
			std::error_code ec;
			const fs::file_status status = fs::symlink_status(at, ec);
			if (status.type() == fs::file_type::not_found) return true;
			if (ec || status.type() == fs::file_type::symlink) return false;
			if (status.type() != fs::file_type::directory) return true;
		}
		return false;
	}

	layer_folder read_folder(const fs::path& layer, std::string_view folder)
	{
		layer_folder out;
		const fs::path relative = fs_paths::path_from_utf8(folder);
		platform::beneath::pinned_root root(layer);
		platform::beneath::list_result listed = platform::beneath::list_beneath(root, relative, k_max_folder_entries);
		if (listed.missing || (!listed.error.empty() && blocked_by_file(layer, relative))) {
			out.missing = true;
			return out;
		}
		if (!listed.error.empty()) {
			out.error = std::move(listed.error);
			return out;
		}
		if (listed.truncated) {
			out.error = std::format("more than {} entries", k_max_folder_entries);
			return out;
		}
		out.names.reserve(listed.entries.size());
		for (const platform::beneath::listed_entry& entry : listed.entries) {
			std::string name = fs_paths::utf8_of(entry.name);
			out.names.emplace(key_of(name), std::move(name));
		}
		return out;
	}

	struct holder
	{
		std::string name;
		bool meta_left = false;
	};

	// What in `listed` holds `name`: the name itself first, then its .resource, then a desc's leftover binary files.
	std::optional<holder> holder_of(const layer_folder& listed, std::string_view name)
	{
		const auto find = [&listed](const std::string& candidate) -> const std::string* {
			const auto found = listed.names.find(key_of(candidate));
			return found == listed.names.end() ? nullptr : &found->second;
		};
		const std::string own{ name };
		if (const std::string* found = find(own)) return holder{ *found, false };
		if (const std::string* found = find(own + std::string{ res::asset_meta_suffix })) return holder{ *found, true };
		constexpr std::string_view desc_suffix = ".desc";
		if (own.size() > desc_suffix.size() && key_of(own).ends_with(desc_suffix)) {
			// The stem rule of res::desc_binary::binary_tag: "<x>.<ext>", so "a.desc" has no binary file.
			const std::string stem = own.substr(0, own.size() - desc_suffix.size());
			const std::size_t dot = stem.find_last_of('.');
			if (dot != std::string::npos && dot != 0 && dot + 1 != stem.size()) {
				for (const std::string_view binary : { std::string_view{ ".bin" }, std::string_view{ ".bin.new" } }) {
					if (const std::string* found = find(stem + std::string{ binary })) return holder{ *found, false };
				}
			}
		}
		return std::nullopt;
	}

	std::pair<std::string_view, std::string_view> split(std::string_view relative)
	{
		const std::size_t slash = relative.rfind('/');
		if (slash == std::string_view::npos) return { {}, relative };
		return { relative.substr(0, slash), relative.substr(slash + 1) };
	}
}

std::optional<res::name_refusal> res::name_taken(const std::vector<fs::path>& layers, std::string_view relative)
{
	const auto [folder, name] = split(relative);
	for (std::size_t i = layers.size(); i-- > 0;) {
		const layer_folder listed = read_folder(layers[i], folder);
		const fs::path where = layers[i] / fs_paths::path_from_utf8(folder);
		if (listed.missing) continue;
		if (!listed.error.empty()) return name_refusal{ i, where, false, listed.error };
		if (const std::optional<holder> found = holder_of(listed, name)) {
			return name_refusal{ i, where / fs_paths::path_from_utf8(found->name), found->meta_left, {} };
		}
	}
	return std::nullopt;
}

res::free_name_result res::free_name(const std::vector<fs::path>& layers, std::string_view folder, std::string_view stem,
	std::string_view suffix, std::string_view separator, const std::function<bool(std::string_view)>& also_taken, int max)
{
	if (max < 1) return { {}, std::format("no name may be tried (at most {})", max) };
	std::vector<layer_folder> listed;
	listed.reserve(layers.size());
	for (std::size_t i = layers.size(); i-- > 0;) {
		layer_folder one = read_folder(layers[i], folder);
		if (!one.error.empty()) {
			return { {}, std::format("'{}' cannot be read: {}", fs_paths::utf8_of(layers[i] / fs_paths::path_from_utf8(folder)), one.error) };
		}
		if (!one.missing) listed.push_back(std::move(one));
	}
	for (int number = 1; number <= max; ++number) {
		std::string name = number == 1 ? std::format("{}{}", stem, suffix) : std::format("{}{}{}{}", stem, separator, number, suffix);
		const std::string relative = folder.empty() ? name : std::format("{}/{}", folder, name);
		if (const std::string problem = portable_path_problem(relative); !problem.empty()) {
			return { {}, std::format("'{}' is no portable name: {}", relative, problem) };
		}
		if (name.size() > max_sanitized_part_length) {
			return { {}, std::format("'{}' is longer than {} bytes", name, max_sanitized_part_length) };
		}
		bool taken = false;
		for (const layer_folder& one : listed) {
			if (holder_of(one, name)) {
				taken = true;
				break;
			}
		}
		if (taken || (also_taken && also_taken(relative))) continue;
		return { std::move(name), {} };
	}
	return { {}, std::format("'{}{}' and every number up to {} are taken", stem, suffix, max) };
}
