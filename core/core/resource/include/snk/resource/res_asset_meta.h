#pragma once
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
