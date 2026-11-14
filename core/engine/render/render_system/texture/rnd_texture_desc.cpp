#include <snk/render/texture/rnd_texture_desc.h>
#include <snk/resource/res_diagnostics.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <boost/json.hpp>

namespace rnd::driver
{
	namespace {
		struct usage_name
		{
			texture_usage flag;
			std::string_view name;
		};

		constexpr usage_name k_usage_names[] = {
			{texture_usage::sampled, "sampled"},
			{texture_usage::storage, "storage"},
			{texture_usage::color_target, "color_target"},
			{texture_usage::depth_target, "depth_target"},
			{texture_usage::stencil_target, "stencil_target"},
		};

		// A single flag is written as a string (the original format), several as an array of strings.
		json::value txm_usage_to_json(texture_usage usage)
		{
			json::array names;
			for (const auto& entry : k_usage_names) {
				if (has_usage(usage, entry.flag)) {
					names.emplace_back(json::string{entry.name});
				}
			}
			if (names.empty()) {
				return json::string{"unknown"};
			}
			if (names.size() == 1) {
				return names.front();
			}
			return names;
		}

		texture_usage txm_usage_from_string(const std::string_view& usage)
		{
			for (const auto& entry : k_usage_names) {
				if (usage == entry.name) {
					return entry.flag;
				}
			}
			return texture_usage::none;
		}

		texture_usage txm_usage_from_json(const json::value& usage)
		{
			if (usage.is_string()) {
				return txm_usage_from_string(usage.as_string());
			}
			texture_usage result = texture_usage::none;
			if (usage.is_array()) {
				for (const auto& item : usage.as_array()) {
					if (item.is_string()) {
						result |= txm_usage_from_string(item.as_string());
					}
				}
			}
			return result;
		}

		std::string txm_wrap_to_string(texture_header::WRAPPING wrap)
		{
			switch (wrap)
			{
			case texture_header::WRAPPING::CLAMP_TO_EDGE: return "clamp_to_edge";
			case texture_header::WRAPPING::REPEAT: return "repeat";
			case texture_header::WRAPPING::CLAMP_TO_BORDER: return "clamp_to_border";
			case texture_header::WRAPPING::REPEAT_MIRROR: return "repeat_mirror";
			}
			return "unknown";
		}

		texture_header::WRAPPING txm_wrap_from_string(const std::string_view& wrap)
		{
			if (wrap == "clamp_to_edge") return texture_header::WRAPPING::CLAMP_TO_EDGE;
			if (wrap == "repeat") return texture_header::WRAPPING::REPEAT;
			if (wrap == "clamp_to_border") return texture_header::WRAPPING::CLAMP_TO_BORDER;
			if (wrap == "repeat_mirror") return texture_header::WRAPPING::REPEAT_MIRROR;
			return texture_header::WRAPPING::CLAMP_TO_EDGE;
		}

		std::string txm_filtering_to_string(texture_header::FILTERING filtering)
		{
			switch (filtering)
			{
			case texture_header::FILTERING::NEAREST: return "nearest";
			case texture_header::FILTERING::LINEAR: return "linear";
			case texture_header::FILTERING::LINEAR_MIPMAP: return "linear_mipmap_nearest";
			}
			return "unknown";
		}

		texture_header::FILTERING txm_filtering_from_string(const std::string_view& filtering)
		{
			if (filtering == "nearest") return texture_header::FILTERING::NEAREST;
			if (filtering == "linear") return texture_header::FILTERING::LINEAR;
			if (filtering == "linear_mipmap_nearest") return texture_header::FILTERING::LINEAR_MIPMAP;
			return texture_header::FILTERING::NEAREST;
		}

		std::string txm_data_type_to_string(texture_header::TYPE type)
		{
			switch (type)
			{
			case texture_header::TYPE::R8: return "R8";
			case texture_header::TYPE::RGB8: return "RGB8";
			case texture_header::TYPE::RGBA8: return "RGBA8";
			case texture_header::TYPE::RGBA12: return "RGBA12";
			case texture_header::TYPE::RGBA16: return "RGBA16";
			case texture_header::TYPE::RGBA16F: return "RGBA16F";
			case texture_header::TYPE::RGBA32F: return "RGBA32F";
			case texture_header::TYPE::R32I: return "R32I";
			case texture_header::TYPE::R32F: return "R32F";
			case texture_header::TYPE::D32F_S8: return "D32F_S8";
			case texture_header::TYPE::D24_S8: return "D24_S8";
			case texture_header::TYPE::D32: return "D32";
			case texture_header::TYPE::D24: return "D24";
			case texture_header::TYPE::D16: return "D16";
			case texture_header::TYPE::D32F: return "D32F";
			case texture_header::TYPE::S1: return "S1";
			case texture_header::TYPE::S4: return "S4";
			case texture_header::TYPE::S8: return "S8";
			}
			return "unknown";
		}

		// Characters that hide or reorder text: bidi controls, line/paragraph separators, zero-width and soft hyphen.
		constexpr bool is_invisible_code_point(std::uint32_t cp)
		{
			return cp == 0x00AD || cp == 0x061C || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E)
				|| cp == 0x2060 || (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF;
		}

		// Text from a file for an error message: at most max_bytes, cut between code points, with every control,
		// invisible or malformed piece shown as '?'.
		std::string printable_text(std::string_view value, std::size_t max_bytes)
		{
			std::string shown;
			shown.reserve(std::min(value.size(), max_bytes));
			std::size_t i = 0;
			while (i < value.size()) {
				const auto lead = static_cast<unsigned char>(value[i]);
				std::size_t len = 0;
				if (lead < 0x80) len = 1;
				else if (lead >= 0xC2 && lead <= 0xDF) len = 2;
				else if (lead >= 0xE0 && lead <= 0xEF) len = 3;
				else if (lead >= 0xF0 && lead <= 0xF4) len = 4;

				bool valid = len != 0 && i + len <= value.size();
				std::uint32_t cp = len == 1 ? lead : (lead & (0x7Fu >> len));
				for (std::size_t k = 1; valid && k < len; ++k) {
					const auto next = static_cast<unsigned char>(value[i + k]);
					valid = (next & 0xC0) == 0x80;
					cp = (cp << 6) | (next & 0x3Fu);
				}
				if (valid && ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))) {
					valid = false;
				}

				if (!valid) {
					if (shown.size() + 1 > max_bytes) break;
					shown.push_back('?');
					++i;
					continue;
				}
				const bool hidden = cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || is_invisible_code_point(cp);
				const std::size_t out_len = hidden ? 1 : len;
				if (shown.size() + out_len > max_bytes) break;
				if (hidden) shown.push_back('?');
				else shown.append(value.substr(i, len));
				i += len;
			}
			return shown;
		}

		// The value comes from a file: a name this build does not know fails the load of that file. It never asserts (an
		// abort in Debug) and never goes on with a format nobody asked for (#930).
		[[noreturn]] void unknown_value(std::string_view json_path, std::string_view what, std::string_view value)
		{
			const std::string shown = printable_text(value, 64);
			throw res::load_error(res::diagnostic_kind::load, json_path, std::format("unknown texture {} '{}'", what, shown));
		}

		texture_header::TYPE txm_data_type_from_string(const std::string_view& type)
		{
			if (type == "R8") return texture_header::TYPE::R8;
			if (type == "RGB8") return texture_header::TYPE::RGB8;
			if (type == "RGBA8") return texture_header::TYPE::RGBA8;
			if (type == "RGBA12") return texture_header::TYPE::RGBA12;
			if (type == "RGBA16") return texture_header::TYPE::RGBA16;
			if (type == "RGBA16F") return texture_header::TYPE::RGBA16F;
			if (type == "RGBA32F") return texture_header::TYPE::RGBA32F;
			if (type == "R32I") return texture_header::TYPE::R32I;
			if (type == "R32F") return texture_header::TYPE::R32F;
			if (type == "D32F_S8") return texture_header::TYPE::D32F_S8;
			if (type == "D24_S8") return texture_header::TYPE::D24_S8;
			if (type == "D32") return texture_header::TYPE::D32;
			if (type == "D24") return texture_header::TYPE::D24;
			if (type == "D16") return texture_header::TYPE::D16;
			if (type == "D32F") return texture_header::TYPE::D32F;
			if (type == "S1") return texture_header::TYPE::S1;
			if (type == "S4") return texture_header::TYPE::S4;
			if (type == "S8") return texture_header::TYPE::S8;
			unknown_value("/header/data/format", "format", type);
		}

		std::string txm_type_to_string(TEXTURE_TYPE type)
		{
			switch (type)
			{
			case TEXTURE_TYPE::TEXTURE_1D: return "texture_1d";
			case TEXTURE_TYPE::TEXTURE_2D: return "texture_2d";
			case TEXTURE_TYPE::TEXTURE_3D: return "texture_3d";
			case TEXTURE_TYPE::TEXTURE_CUBE_MAP: return "texture_cube_map";
			case TEXTURE_TYPE::TEXTURE_CUBE_ARRAY: return "texture_cube_array";
			case TEXTURE_TYPE::RENDERBUFFER: return "renderbuffer";
			case TEXTURE_TYPE::TEXTURE_1D_ARRAY: return "texture_1d_array";
			case TEXTURE_TYPE::TEXTURE_2D_ARRAY: return "texture_2d_array";
			}
			return "unknown";
		}

		TEXTURE_TYPE txm_type_from_string(const std::string_view& type)
		{
			if (type == "texture_1d") return TEXTURE_TYPE::TEXTURE_1D;
			if (type == "texture_2d") return TEXTURE_TYPE::TEXTURE_2D;
			if (type == "texture_3d") return TEXTURE_TYPE::TEXTURE_3D;
			if (type == "texture_cube_map") return TEXTURE_TYPE::TEXTURE_CUBE_MAP;
			if (type == "texture_1d_array") return TEXTURE_TYPE::TEXTURE_1D_ARRAY;
			if (type == "texture_2d_array") return TEXTURE_TYPE::TEXTURE_2D_ARRAY;
			if (type == "texture_cube_array") return TEXTURE_TYPE::TEXTURE_CUBE_ARRAY;
			if (type == "renderbuffer") return TEXTURE_TYPE::RENDERBUFFER;
			unknown_value("/header/type", "type", type);
		}
	}

	void tag_invoke(json::value_from_tag, json::value& out, const texture_header& c)
	{
		out = {
			{"type", txm_type_to_string(c.type)},
			{"usage", txm_usage_to_json(c.usage)},
			{"wrap", txm_wrap_to_string(c.wrap)},
			{"mag", txm_filtering_to_string(c.mag)},
			{"min", txm_filtering_to_string(c.min)},
			{"data", 
				{
					{"extent", {
							{"width", c.data.extent.width},
							{"height", c.data.extent.height},
							{"depth", c.data.extent.depth}
						}
					},
					{"mip_levels", c.data.mip_levels},
					{"format", txm_data_type_to_string(c.data.format)},
				}
			}
		};
	}

	texture_header tag_invoke(json::value_to_tag<texture_header>, const json::value& value, const driver::texture_header& header)
	{
		if (!value.is_object())
		{
			return {};
		}

		texture_header c = header;

		auto& obj = value.as_object();
		if (obj.contains("type"))
			c.type = txm_type_from_string(obj.at("type").as_string());
		if (obj.contains("usage"))
			c.usage = txm_usage_from_json(obj.at("usage"));
		if (obj.contains("wrap"))
			c.wrap = txm_wrap_from_string(obj.at("wrap").as_string());
		if (obj.contains("mag"))
			c.mag = txm_filtering_from_string(obj.at("mag").as_string());
		if (obj.contains("min"))
			c.min = txm_filtering_from_string(obj.at("min").as_string());

		if (!obj.contains("data")) {
			return c;
		}

		auto& data = obj.at("data").as_object();

		if (data.contains("extent")) {
			auto& extent = data.at("extent").as_object();
			if (extent.contains("width"))
				c.data.extent.width = static_cast<uint32_t>(extent.at("width").as_int64());
			if (extent.contains("height"))
				c.data.extent.height = static_cast<uint32_t>(extent.at("height").as_int64());
			if (extent.contains("depth"))
				c.data.extent.depth = static_cast<uint32_t>(extent.at("depth").as_int64());
		}

		if (data.contains("mip_levels"))
			c.data.mip_levels = static_cast<uint32_t>(data.at("mip_levels").as_int64());
		if (data.contains("format"))
			c.data.format = txm_data_type_from_string(data.at("format").as_string());

		return c;
	}
}

void rnd::texture_desc::deserialize(desc::desc_system& system, const json::object& resource)
{
	if (resource.contains("name"))
		txm_name = resource.at("name").as_string();
	if (resource.contains("header"))
		header = json::value_to<driver::texture_header>(resource.at("header"), header);
}

void rnd::texture_desc::serialize(json::object& resource) const
{
	/*
		{
			name: "texture",
			header: {
				usage: "texture_usage" | ["texture_usage", ...],
				wrap: "wrapping",
				mag: "filtering",
				min: "filtering",
				data: {
					extent: {
						width: 1024,
						height: 1024,
						depth: 1
					},
					format: "texture_format"
				}
			}
			
		}
	*/
	resource["__type"] = "texture_desc";
	resource["name"] = txm_name;
	resource["header"] = json::value_from(header);
}
