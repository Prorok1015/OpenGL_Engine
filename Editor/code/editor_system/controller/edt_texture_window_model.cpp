#include "edt_texture_window_model.h"
#include "edt_data_document.h"
#include <snk/desc/desc_json_ops.h>
#include <boost/json.hpp>
#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace json = boost::json;

namespace
{
	constexpr std::array<std::string_view, 4> k_wraps{ "clamp_to_edge", "repeat", "clamp_to_border", "repeat_mirror" };
	constexpr std::array<std::string_view, 3> k_filters{ "nearest", "linear", "linear_mipmap_nearest" };

	// rnd::driver::texture_header as it is built: what a header without the key reads.
	json::object default_header()
	{
		return json::object{
			{ "type", "texture_2d" },
			{ "usage", "sampled" },
			{ "wrap", "clamp_to_edge" },
			{ "mag", "linear" },
			{ "min", "linear" },
			{ "data", json::object{
				{ "extent", json::object{ { "width", 1 }, { "height", 1 }, { "depth", 1 } } },
				{ "mip_levels", 1 },
				{ "format", "RGBA8" } } },
		};
	}

	std::string text_at(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		return value && value->is_string() ? std::string(value->as_string()) : std::string{};
	}

	bool is_option(std::span<const std::string_view> options, std::string_view value)
	{
		return std::ranges::find(options, value) != options.end();
	}

	std::int64_t whole_at(const json::object& object, std::string_view key)
	{
		const json::value* value = object.if_contains(key);
		if (!value) return 0;
		if (value->is_int64()) return value->as_int64();
		if (value->is_uint64()) return static_cast<std::int64_t>(value->as_uint64());
		return 0;
	}
}

json::object edt::effective_texture_header(const json::value& body)
{
	json::object header = default_header();
	const json::value* own = body.is_object() ? body.as_object().if_contains("header") : nullptr;
	if (own && own->is_object()) desc::json_deep_merge(header, own->as_object());
	return header;
}

edt::texture_source edt::texture_source_of(const json::value& body)
{
	texture_source out;
	if (!body.is_object()) return out;
	const json::object& object = body.as_object();
	if (const json::value* pixels = object.if_contains("pixels"); pixels && pixels->is_object()) {
		out.from = texture_source::kind::inline_pixels;
		out.width = whole_at(pixels->as_object(), "width");
		out.height = whole_at(pixels->as_object(), "height");
		out.format = text_at(pixels->as_object(), "format");
		return out;
	}
	if (const json::value* data = object.if_contains("data")) {
		// A path, or {guid, path} (#920).
		if (data->is_string() && !data->as_string().empty()) {
			out.from = texture_source::kind::file;
			out.file = std::string(data->as_string());
		} else if (data->is_object() && !text_at(data->as_object(), "path").empty()) {
			out.from = texture_source::kind::file;
			out.file = text_at(data->as_object(), "path");
		}
	}
	return out;
}

std::string edt::texture_source_text(const texture_source& source)
{
	switch (source.from) {
	case texture_source::kind::file: return source.file;
	case texture_source::kind::inline_pixels: return std::format("inline {}x{} {}", source.width, source.height, source.format);
	case texture_source::kind::none: break;
	}
	return "none";
}

std::span<const std::string_view> edt::texture_wrap_options()
{
	return k_wraps;
}

std::span<const std::string_view> edt::texture_filter_options()
{
	return k_filters;
}

edt::texture_window_model::texture_window_model(data_document& document)
	: m_document(document)
{
}

json::object edt::texture_window_model::effective_header() const
{
	return effective_texture_header(m_document.shown());
}

std::string edt::texture_window_model::wrap() const
{
	return text_at(effective_header(), "wrap");
}

std::string edt::texture_window_model::min_filter() const
{
	return text_at(effective_header(), "min");
}

std::string edt::texture_window_model::mag_filter() const
{
	return text_at(effective_header(), "mag");
}

int edt::texture_window_model::mip_levels() const
{
	const json::object header = effective_header();
	const json::value* data = header.if_contains("data");
	return data && data->is_object() ? static_cast<int>(whole_at(data->as_object(), "mip_levels")) : 1;
}

bool edt::texture_window_model::write_header(const std::function<void(json::object& header)>& change)
{
	json::object header = effective_header();
	const json::object before = header;
	change(header);
	if (header == before) return false;
	// A choice is a click, not a drag: a record of its own, apart from an edit of a field still pending.
	m_document.commit_pending();
	const bool changed = m_document.edit([&header](json::value& shown) {
		if (!shown.is_object()) return false;
		shown.as_object()["header"] = header;
		return true;
	});
	m_document.commit_pending();
	return changed;
}

bool edt::texture_window_model::set_wrap(std::string_view wrap)
{
	if (!is_option(k_wraps, wrap)) return false;
	return write_header([wrap](json::object& header) { header["wrap"] = wrap; });
}

bool edt::texture_window_model::set_min(std::string_view filter)
{
	if (!is_option(k_filters, filter)) return false;
	return write_header([filter](json::object& header) { header["min"] = filter; });
}

bool edt::texture_window_model::set_mag(std::string_view filter)
{
	if (!is_option(k_filters, filter)) return false;
	return write_header([filter](json::object& header) { header["mag"] = filter; });
}

bool edt::texture_window_model::set_mip_levels(int levels)
{
	const int wanted = std::max(levels, 1);
	return write_header([wanted](json::object& header) {
		json::value& data = header["data"];
		if (!data.is_object()) data = json::object{};
		data.as_object()["mip_levels"] = wanted;
	});
}

edt::texture_source edt::texture_window_model::source() const
{
	return texture_source_of(m_document.shown());
}

bool edt::texture_window_model::use_file(const res::tag& picture)
{
	if (!picture.is_valid()) return false;
	m_document.commit_pending();
	const bool changed = m_document.edit([&picture](json::value& shown) {
		if (!shown.is_object()) return false;
		json::object& object = shown.as_object();
		object["data"] = std::string(picture.view());
		object["pixels"] = nullptr;
		return true;
	});
	m_document.commit_pending();
	return changed;
}
