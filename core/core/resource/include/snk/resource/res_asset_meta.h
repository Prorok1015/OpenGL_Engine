#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <boost/json/object.hpp>
#include <snk/common/snk_api.h>
#include <snk/resource/res_asset_id.h>

namespace json = boost::json;

// The .resource sidecar next to every file of the resource tree (EPIC-57 section 4.2): the only source of the file's asset_id.
namespace res
{
	inline constexpr std::int64_t asset_meta_version = 1;
	// Also the pseudo-type under which project migration keeps the steps of a sidecar (EPIC-58 3.4); a step that
	// raises the version raises asset_meta_version with it.
	inline constexpr std::string_view asset_meta_type = "resource_meta";
	inline constexpr std::string_view asset_meta_suffix = ".resource";

	// A name or path ending in ".resource" in any letter case: one spelling rule for the editor and the database, so a
	// sidecar renamed on Windows is still recognized everywhere.
	inline bool is_asset_meta_name(std::string_view name)
	{
		if (name.size() < asset_meta_suffix.size()) return false;
		const std::string_view tail = name.substr(name.size() - asset_meta_suffix.size());
		for (std::size_t i = 0; i < tail.size(); ++i) {
			const char c = tail[i] >= 'A' && tail[i] <= 'Z' ? static_cast<char>(tail[i] - 'A' + 'a') : tail[i];
			if (c != asset_meta_suffix[i]) return false;
		}
		return true;
	}

	// Not an asset: dot files (.gitkeep, .DS_Store, .x.swp) and well-known temporary or system names (Thumbs.db,
	// desktop.ini, ~$x, x.tmp, x~). The asset database does not list them and no .resource is made for them.
	inline bool is_ignored_asset_name(std::string_view name)
	{
		const auto equals_lower = [](std::string_view text, std::string_view lower) {
			if (text.size() != lower.size()) {
				return false;
			}
			for (std::size_t i = 0; i < text.size(); ++i) {
				const char c = text[i] >= 'A' && text[i] <= 'Z' ? static_cast<char>(text[i] - 'A' + 'a') : text[i];
				if (c != lower[i]) {
					return false;
				}
			}
			return true;
		};
		if (name.empty() || name.front() == '.' || name.starts_with("~$") || name.back() == '~') {
			return true;
		}
		if (equals_lower(name, "thumbs.db") || equals_lower(name, "desktop.ini")) {
			return true;
		}
		constexpr std::string_view tmp_suffix = ".tmp";
		return name.size() > tmp_suffix.size() && equals_lower(name.substr(name.size() - tmp_suffix.size()), tmp_suffix);
	}

	struct asset_meta
	{
		asset_id guid;
		std::string importer;
		json::object settings;
		std::vector<std::string> flags;
	};

	enum class asset_meta_error_kind
	{
		malformed,
		// __version is above asset_meta_version: written by a newer engine, which this one must not rewrite.
		newer_than_engine,
	};

	struct asset_meta_error
	{
		asset_meta_error_kind kind = asset_meta_error_kind::malformed;
		std::string field_path; // "__guid", "flags[2]"; empty for the document itself
		std::string message;
	};

	struct asset_meta_result
	{
		std::optional<asset_meta> meta;
		asset_meta_error error; // meaningful only without meta

		explicit operator bool() const { return meta.has_value(); }
	};

	// Every key is required, and a key this version does not know is an error.
	SNK_API asset_meta_result read_asset_meta(std::string_view text);

	// Keys in a fixed order (__type, __version, __guid, importer, settings, flags) in the project's JSON style, so writing
	// what read_asset_meta read gives back the same bytes.
	SNK_API std::string write_asset_meta(const asset_meta& meta);
}
