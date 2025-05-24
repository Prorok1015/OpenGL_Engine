#include "rnd_texture_desc.h"

namespace rnd::driver
{
	namespace {
		std::string_view txm_usage_to_string(TEXTURE_USAGE usage)
		{
			switch (usage)
			{
			case TEXTURE_USAGE::COLOR_TARGET: return "color_target";
			case TEXTURE_USAGE::DEPTH_TARGET: return "depth_target";
			case TEXTURE_USAGE::SAMPLED: return "sampled";
			case TEXTURE_USAGE::STENCIL_TARGET: return "stencil_target";
			case TEXTURE_USAGE::STORAGE: return "storage";
			}
			return "unknown";
		}

		TEXTURE_USAGE txm_usage_from_string(const std::string_view& usage)
		{
			if (usage == "color_target") return TEXTURE_USAGE::COLOR_TARGET;
			if (usage == "depth_target") return TEXTURE_USAGE::DEPTH_TARGET;
			if (usage == "sampled") return TEXTURE_USAGE::SAMPLED;
			if (usage == "stencil_target") return TEXTURE_USAGE::STENCIL_TARGET;
			if (usage == "storage") return TEXTURE_USAGE::STORAGE;
			return TEXTURE_USAGE::NONE;
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
			ASSERT_FAIL("Unknown texture type: {0}", type);
			return texture_header::TYPE::RGB8;
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
			}
			return "unknown";
		}

		TEXTURE_TYPE txm_type_from_string(const std::string_view& type)
		{
			if (type == "texture_1d") return TEXTURE_TYPE::TEXTURE_1D;
			if (type == "texture_2d") return TEXTURE_TYPE::TEXTURE_2D;
			if (type == "texture_3d") return TEXTURE_TYPE::TEXTURE_3D;
			if (type == "texture_cube_map") return TEXTURE_TYPE::TEXTURE_CUBE_MAP;
			if (type == "texture_cube_array") return TEXTURE_TYPE::TEXTURE_CUBE_ARRAY;
			if (type == "renderbuffer") return TEXTURE_TYPE::RENDERBUFFER;
			ASSERT_FAIL("Unknown texture type: {0}", type);
			return TEXTURE_TYPE::TEXTURE_2D;
		}
	}

	void tag_invoke(json::value_from_tag, json::value& out, const texture_header& c)
	{
		out = {
			{"type", txm_type_to_string(c.type)},
			{"usage", txm_usage_to_string(c.usage)},
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
			c.usage = txm_usage_from_string(obj.at("usage").as_string());
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
				c.data.extent.width = extent.at("width").as_int64();
			if (extent.contains("height"))
				c.data.extent.height = extent.at("height").as_int64();
			if (extent.contains("depth"))
				c.data.extent.depth = extent.at("depth").as_int64();
		}

		if (data.contains("mip_levels"))
			c.data.mip_levels = data.at("mip_levels").as_int64();
		if (data.contains("format"))
			c.data.format = txm_data_type_from_string(data.at("format").as_string());

		return c;
	}
}

void rnd::texture_desc::deserialize(desc::desc_system& system, const json::object& resource)
{
	if (resource.contains("name"))
		txm_name = resource.at("name").as_string();
	if (resource.contains("data"))
		txm_tag = res::tag{ resource.at("data").as_string() };
	if (resource.contains("header"))
		header = json::value_to<driver::texture_header>(resource.at("header"), header);
}

void rnd::texture_desc::serialize(json::object& resource) const
{
	/*
		{
			name: "texture",
			header: {
				usage: "texture_usage",
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
			data: "res://path/to/texture.png",
			
		}
	*/

	resource["name"] = txm_name;
	resource["header"] = json::value_from(header);
	resource["data"] = json::value_from(txm_tag.get_full());
}
