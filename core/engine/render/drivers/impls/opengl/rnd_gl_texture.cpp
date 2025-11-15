#include "rnd_gl_texture.h"
#include "open_gl_specific.h"
#include <algorithm>

namespace rnd::driver::gl 
{
	namespace 
	{
		struct gl_format { GLenum internal_format, format, type; };

		const GLint gTextureFilteringToGlFiltering[] =
		{
			GL_NEAREST,
			GL_LINEAR,
			GL_LINEAR_MIPMAP_LINEAR,
		};

		const GLint gTextureWrappingToGlWrapping[] =
		{
			GL_REPEAT,
			GL_MIRRORED_REPEAT,
			GL_CLAMP_TO_EDGE,
			GL_CLAMP_TO_BORDER,
		};

		GLint to_gl_filter(texture_header::FILTERING filtering)
		{
			return gTextureFilteringToGlFiltering[static_cast<int>(filtering)];
		}

		GLint to_gl_wrap(texture_header::WRAPPING wrapping)
		{
			return gTextureWrappingToGlWrapping[static_cast<int>(wrapping)];
		}

		GLenum texture_type_to_gl_target(TEXTURE_TYPE type) 
		{
			switch (type) 
			{
				case TEXTURE_TYPE::TEXTURE_1D: return GL_TEXTURE_1D;
				case TEXTURE_TYPE::TEXTURE_2D: return GL_TEXTURE_2D;
				case TEXTURE_TYPE::TEXTURE_3D: return GL_TEXTURE_3D;
				case TEXTURE_TYPE::TEXTURE_CUBE_MAP: return GL_TEXTURE_CUBE_MAP;
				case TEXTURE_TYPE::TEXTURE_1D_ARRAY: return GL_TEXTURE_1D_ARRAY;
				case TEXTURE_TYPE::TEXTURE_2D_ARRAY: return GL_TEXTURE_2D_ARRAY;
				case TEXTURE_TYPE::TEXTURE_CUBE_ARRAY: return GL_TEXTURE_CUBE_MAP_ARRAY;
				case TEXTURE_TYPE::RENDERBUFFER: return GL_RENDERBUFFER;
				default: throw std::runtime_error("Unknown texture type");
			}
		}

		GLenum get_gl_attachment_type(texture_header::TYPE format) 
		{
			switch (format) 
			{
				case texture_header::TYPE::D16:
				case texture_header::TYPE::D24:
				case texture_header::TYPE::D32:
				case texture_header::TYPE::D32F:
					return GL_DEPTH_ATTACHMENT;
				case texture_header::TYPE::D24_S8:
				case texture_header::TYPE::D32F_S8:
					return GL_DEPTH_STENCIL_ATTACHMENT;
				case texture_header::TYPE::S1:
				case texture_header::TYPE::S4:
				case texture_header::TYPE::S8:
					return GL_STENCIL_ATTACHMENT;
				default:
					return GL_COLOR_ATTACHMENT0;
			}
		}

		gl_format get_gl_format(texture_header::TYPE format) 
		{
			switch (format) 
			{
				case texture_header::TYPE::R8: 
					return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
				case texture_header::TYPE::RGB8: 
					return {GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE};
				case texture_header::TYPE::RGBA8: 
					return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
				case texture_header::TYPE::RGBA12: 
					return {GL_RGBA12, GL_RGBA, GL_UNSIGNED_BYTE};
				case texture_header::TYPE::RGBA16: 
					return {GL_RGBA16, GL_RGBA, GL_UNSIGNED_BYTE};
				case texture_header::TYPE::RGBA16F: 
					return {GL_RGBA16F, GL_RGBA, GL_FLOAT};
				case texture_header::TYPE::RGBA32F: 
					return {GL_RGBA32F, GL_RGBA, GL_FLOAT};
				case texture_header::TYPE::R32I: 
					return {GL_R32I, GL_RED_INTEGER, GL_INT};
				case texture_header::TYPE::R32F: 
					return {GL_R32F, GL_RED_INTEGER, GL_FLOAT};
				case texture_header::TYPE::D24_S8: 
					return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8};
				case texture_header::TYPE::D32F: 
					return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT};
				case texture_header::TYPE::D32F_S8: 
					return {GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV};
				case texture_header::TYPE::D16: 
					return {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT};
				case texture_header::TYPE::D24: 
					return {GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT};
				case texture_header::TYPE::D32: 
					return {GL_DEPTH_COMPONENT32, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT};
				case texture_header::TYPE::S1: 
					return {GL_STENCIL_INDEX1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE};
				case texture_header::TYPE::S4: 
					return {GL_STENCIL_INDEX4, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE};
				case texture_header::TYPE::S8: 
					return {GL_STENCIL_INDEX8, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE};
				default: 
					throw std::runtime_error("Unknown texture format");
			}
		}
	}

	texture::texture(const texture_header& hdr)
		: texture_interface(hdr)
		, target(texture_type_to_gl_target(hdr.type))
	{
		create_texture();
		set_storage();

		set_filtering(hdr.min, hdr.mag);
		set_wrapping(hdr.wrap, hdr.wrap, hdr.wrap);

		if (hdr.data.initial_data) {
			update_data(hdr.data.initial_data, 0, 0, 0,
					   hdr.data.extent.width,
					   hdr.data.extent.height,
					   hdr.data.extent.depth);
			
			if (hdr.data.mip_levels > 1) {
				generate_mipmaps();
			}
		}
	}

	texture::~texture() 
	{
		if (header.type == TEXTURE_TYPE::RENDERBUFFER) {
			glDeleteRenderbuffers(1, &id);
		} else {
			glDeleteTextures(1, &id);
		}
		CHECK_GL_ERROR();
	}

	void texture::bind(uint32_t position) 
	{
		if (header.type != TEXTURE_TYPE::RENDERBUFFER) {
			glBindTextureUnit(position, id);
			CHECK_GL_ERROR();
		}
	}

	void texture::unbind(uint32_t position) 
	{
		if (header.type != TEXTURE_TYPE::RENDERBUFFER) {
			glBindTextureUnit(position, 0);
			CHECK_GL_ERROR();
		}
	}

	void texture::create_texture() 
	{
		if (header.type == TEXTURE_TYPE::RENDERBUFFER) {
			glCreateRenderbuffers(1, &id);
		} else {
			glCreateTextures(target, 1, &id);
		}
		CHECK_GL_ERROR();
	}

	void texture::set_storage() 
	{
		const auto& extent = header.data.extent;
		auto [internal_format, format, type] = get_gl_format(header.data.format);

		if (header.type == TEXTURE_TYPE::RENDERBUFFER) {
			glNamedRenderbufferStorage(id, internal_format, extent.width, extent.height);
		}
		else {
			switch (header.type) 
			{
				case TEXTURE_TYPE::TEXTURE_1D:
				case TEXTURE_TYPE::TEXTURE_1D_ARRAY:
					glTextureStorage1D(id, header.data.mip_levels, internal_format, extent.width);
					break;

				case TEXTURE_TYPE::TEXTURE_2D:
				case TEXTURE_TYPE::TEXTURE_CUBE_MAP:
					glTextureStorage2D(id, header.data.mip_levels, internal_format, extent.width, extent.height);
					break;

				case TEXTURE_TYPE::TEXTURE_2D_ARRAY:
				case TEXTURE_TYPE::TEXTURE_3D:
				case TEXTURE_TYPE::TEXTURE_CUBE_ARRAY:
					glTextureStorage3D(id, header.data.mip_levels, internal_format, 
									 extent.width, extent.height, 
									 header.type == TEXTURE_TYPE::TEXTURE_3D ? extent.depth : extent.layers * extent.faces);
					break;
			}
		}
		CHECK_GL_ERROR();
	}

	void texture::update_data(const void* data, uint32_t x_offset, uint32_t y_offset, uint32_t z_offset,
							uint32_t width, uint32_t height, uint32_t depth,
							uint32_t level, uint32_t layer, uint32_t face) 
	{
		if (!data || header.type == TEXTURE_TYPE::RENDERBUFFER) return;

		auto [internal_format, format, type] = get_gl_format(header.data.format);

		switch (header.type) 
		{
			case TEXTURE_TYPE::TEXTURE_1D:
				glTextureSubImage1D(id, level, x_offset, width, format, type, data);
				break;

			case TEXTURE_TYPE::TEXTURE_2D:
				glTextureSubImage2D(id, level, x_offset, y_offset, width, height, format, type, data);
				break;

			case TEXTURE_TYPE::TEXTURE_3D:
				glTextureSubImage3D(id, level, x_offset, y_offset, z_offset, 
								  width, height, depth, format, type, data);
				break;

			case TEXTURE_TYPE::TEXTURE_CUBE_MAP:
				glTextureSubImage3D(id, level, x_offset, y_offset, face,
								  width, height, 1, format, type, data);
				break;

			case TEXTURE_TYPE::TEXTURE_1D_ARRAY:
				glTextureSubImage2D(id, level, x_offset, layer, width, 1, format, type, data);
				break;

			case TEXTURE_TYPE::TEXTURE_2D_ARRAY:
			case TEXTURE_TYPE::TEXTURE_CUBE_ARRAY:
				glTextureSubImage3D(id, level, x_offset, y_offset, layer * 6 + face,
								  width, height, 1, format, type, data);
				break;
		}
		CHECK_GL_ERROR();
	}

	void texture::generate_mipmaps() 
	{
		if (header.type != TEXTURE_TYPE::RENDERBUFFER) {
			glGenerateTextureMipmap(id);
			CHECK_GL_ERROR();
		}
	}

	void texture::set_filtering(texture_header::FILTERING min, texture_header::FILTERING mag) 
	{
		if (header.type == TEXTURE_TYPE::RENDERBUFFER) return;

		glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, to_gl_filter(min));
		glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, to_gl_filter(mag));
		CHECK_GL_ERROR();
	}

	void texture::set_wrapping(texture_header::WRAPPING wrap_s, 
							 texture_header::WRAPPING wrap_t,
							 texture_header::WRAPPING wrap_r) 
	{
		if (header.type == TEXTURE_TYPE::RENDERBUFFER) return;

		glTextureParameteri(id, GL_TEXTURE_WRAP_S, to_gl_wrap(wrap_s));
		glTextureParameteri(id, GL_TEXTURE_WRAP_T, to_gl_wrap(wrap_t));
		if (header.type == TEXTURE_TYPE::TEXTURE_3D ||
			header.type == TEXTURE_TYPE::TEXTURE_CUBE_MAP ||
			header.type == TEXTURE_TYPE::TEXTURE_CUBE_ARRAY) {
			glTextureParameteri(id, GL_TEXTURE_WRAP_R, to_gl_wrap(wrap_r));
		}
		CHECK_GL_ERROR();
	}

	GLenum texture::get_attachment_type() const 
	{
		return get_gl_attachment_type(header.data.format);
	}
}
