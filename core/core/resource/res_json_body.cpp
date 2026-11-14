#include <snk/resource/res_json_body.h>
#include <snk/resource/res_diagnostics.h>
#include <boost/json/parse.hpp>
#include <boost/json/parse_options.hpp>
#include <format>

std::size_t res::json_byte_limit(std::string_view name)
{
	return name.ends_with(".geom.desc") ? max_geometry_json_bytes : max_json_bytes;
}

json::object res::parse_json_body(std::string_view name, std::span<const std::byte> data, std::size_t limit)
{
	if (data.size() > limit) {
		throw load_error(diagnostic_kind::load, {}, std::format("'{}' of {} bytes is not parsed: at most {} bytes", name, data.size(), limit));
	}
	const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
	json::parse_options options;
	options.max_depth = 128;
	return json::parse(text, {}, options).as_object();
}
