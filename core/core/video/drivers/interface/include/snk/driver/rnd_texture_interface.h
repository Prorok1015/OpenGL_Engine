#pragma once
#include "common.h"
#include "ds/ds_bit_frags.hpp"
#include <glm/glm.hpp>

namespace rnd::driver
{
	// Texture type (physical representation)
	enum class TEXTURE_TYPE
	{
		TEXTURE_1D,
		TEXTURE_2D,
		TEXTURE_3D,
		TEXTURE_CUBE_MAP,
		TEXTURE_1D_ARRAY,
		TEXTURE_2D_ARRAY,
		TEXTURE_CUBE_ARRAY,
		RENDERBUFFER
	};

	// Texture usage (how it will be used)
	enum class TEXTURE_USAGE
	{
		NONE = 0,
		SAMPLED,		// Can be used as a sampler in shader
		STORAGE,		// Can be used as storage image
		COLOR_TARGET,	// Can be used as color attachment
		DEPTH_TARGET,	// Can be used as depth attachment
		STENCIL_TARGET, // Can be used as stencil attachment
	};


	// Extended texture header
	struct texture_header
	{
		enum class TYPE
		{
			R8,
			RGB8,
			RGBA8,
			RGBA12,
			RGBA16,
			RGBA16F,
			RGBA32F,
			R32I,
			R32F,
			D32F_S8,
			D24_S8,
			D32,
			D24,
			D16,
			D32F,
			S1,
			S4,
			S8
		};

		enum class FILTERING
		{
			NEAREST,
			LINEAR,
			LINEAR_MIPMAP
		};

		enum class WRAPPING
		{
			REPEAT,
			REPEAT_MIRROR,
			CLAMP_TO_EDGE,
			CLAMP_TO_BORDER
		};

		struct texture_extent
		{
			uint32_t width = 1;
			uint32_t height = 1;
			uint32_t depth = 1;     // For 3D textures or arrays
			uint32_t layers = 1;    // Number of layers for array textures
			uint32_t faces = 1;     // 6 for cubemap, 1 for others
		};

		struct texture_data
		{
			texture_extent extent;
			TYPE format = TYPE::RGBA8;
			uint32_t mip_levels = 1;
			const void* initial_data = nullptr;
		};

		std::string name = "unknown";
		TEXTURE_TYPE type = TEXTURE_TYPE::TEXTURE_2D;
		TEXTURE_USAGE usage = TEXTURE_USAGE::SAMPLED;
		texture_data data;
		FILTERING min = FILTERING::LINEAR;
		FILTERING mag = FILTERING::LINEAR;
		WRAPPING wrap = WRAPPING::CLAMP_TO_EDGE;
	};

	class texture_interface
	{
	public:
		texture_interface(const texture_header& hdr)
			: header(hdr) {}
		virtual ~texture_interface() = default;

		// Base operations
		virtual void bind(uint32_t position) = 0;
		virtual void unbind(uint32_t position) = 0;

		// Texture information
		TEXTURE_TYPE get_type() const { return header.type; }
		TEXTURE_USAGE get_usage() const { return header.usage; }
		bool has_usage(TEXTURE_USAGE usage) const { 
			return header.usage == usage;
		}

		// Texture dimensions
		uint32_t width() const { return header.data.extent.width; }
		uint32_t height() const { return header.data.extent.height; }
		uint32_t depth() const { return header.data.extent.depth; }
		uint32_t layers() const { return header.data.extent.layers; }
		uint32_t faces() const { return header.data.extent.faces; }
		uint32_t mip_levels() const { return header.data.mip_levels; }

		// Texture parameters
		virtual void set_filtering(texture_header::FILTERING min, texture_header::FILTERING mag) = 0;
		virtual void set_wrapping(texture_header::WRAPPING wrap_s, 
								texture_header::WRAPPING wrap_t, 
								texture_header::WRAPPING wrap_r = texture_header::WRAPPING::CLAMP_TO_EDGE) = 0;

		// Update data
		virtual void update_data(const void* data, 
							   uint32_t x_offset, 
							   uint32_t y_offset, 
							   uint32_t z_offset,
							   uint32_t width, 
							   uint32_t height, 
							   uint32_t depth,
							   uint32_t level = 0,
							   uint32_t layer = 0,
							   uint32_t face = 0) = 0;

		// Generate mipmaps
		virtual void generate_mipmaps() = 0;

	protected:
		texture_header header;
	};
}