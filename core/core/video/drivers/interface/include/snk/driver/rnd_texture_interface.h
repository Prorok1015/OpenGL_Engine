#pragma once
#include <snk/common/common.h>
#include <snk/common/ds/ds_bit_frags.hpp>
#include <cstdint>
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

	// Texture usage (how it will be used): a bit mask, a texture may be e.g. both a color target and sampled
	enum class texture_usage : uint8_t
	{
		none = 0,
		sampled = 1,		// Can be used as a sampler in shader
		storage = 2,		// Can be used as storage image
		color_target = 4,	// Can be used as color attachment
		depth_target = 8,	// Can be used as depth attachment
		stencil_target = 16	// Can be used as stencil attachment
	};

	constexpr texture_usage operator|(texture_usage lhs, texture_usage rhs) noexcept
	{
		return static_cast<texture_usage>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
	}

	constexpr texture_usage operator&(texture_usage lhs, texture_usage rhs) noexcept
	{
		return static_cast<texture_usage>(static_cast<uint8_t>(lhs) & static_cast<uint8_t>(rhs));
	}

	constexpr texture_usage& operator|=(texture_usage& lhs, texture_usage rhs) noexcept
	{
		return lhs = lhs | rhs;
	}

	// True when `usage` contains every bit of `required` (and `required` is not empty).
	constexpr bool has_usage(texture_usage usage, texture_usage required) noexcept
	{
		return required != texture_usage::none && (usage & required) == required;
	}

	// True when `usage` contains at least one bit of `any_of`.
	constexpr bool has_any_usage(texture_usage usage, texture_usage any_of) noexcept
	{
		return (usage & any_of) != texture_usage::none;
	}


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
		texture_usage usage = texture_usage::sampled;
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

		// The texture a backend-specific consumer may downcast; decorators (validation) return the one they wrap.
		virtual texture_interface* native() { return this; }

		// Base operations
		virtual void bind(uint32_t position) = 0;
		virtual void unbind(uint32_t position) = 0;

		// Texture information
		TEXTURE_TYPE get_type() const { return header.type; }
		texture_usage get_usage() const { return header.usage; }
		bool has_usage(texture_usage required) const { return rnd::driver::has_usage(header.usage, required); }

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