#pragma once
#include <cstddef>
#include <span>
#include <string_view>
#include <boost/json/object.hpp>
#include <snk/common/snk_api.h>

namespace json = boost::json;

// How large a JSON file of untrusted data may be before it is parsed (#928): parsed, it takes several times its size
// in memory, and the resolver reads files of any size.
namespace res
{
	inline constexpr std::size_t max_json_bytes = std::size_t{ 64 } * 1024 * 1024;
	// Imported geometry (<model>.geom.desc) holds every vertex and index of a model as text.
	inline constexpr std::size_t max_geometry_json_bytes = std::size_t{ 256 } * 1024 * 1024;

	// The limit of a file by its name or tag text: max_geometry_json_bytes for "*.geom.desc", max_json_bytes otherwise.
	SNK_API std::size_t json_byte_limit(std::string_view name);

	// The JSON object in `data`. More than `limit` bytes: load_error (kind `load`), and nothing is parsed. A text that
	// is not a JSON object throws what boost::json throws.
	SNK_API json::object parse_json_body(std::string_view name, std::span<const std::byte> data, std::size_t limit);

	inline json::object parse_json_body(std::string_view name, std::span<const std::byte> data)
	{
		return parse_json_body(name, data, json_byte_limit(name));
	}
}
