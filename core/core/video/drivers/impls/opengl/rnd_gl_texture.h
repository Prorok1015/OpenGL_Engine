#pragma once
#include <snk/driver/rnd_texture_interface.h>
#include <glad/glad.h>
#include <cstdint>
#include <memory>

namespace rnd::driver::gl
{
	class framebuffer_registry;

	class texture : public rnd::driver::texture_interface
	{
	public:
		// Constructor takes texture header
		// `framebuffers` is told when the texture is destroyed, so it drops the framebuffers that use it.
		explicit texture(const texture_header& hdr, std::shared_ptr<framebuffer_registry> framebuffers = nullptr);
		virtual ~texture() override;

		// Interface methods
		virtual void bind(uint32_t position) override;
		virtual void unbind(uint32_t position) override;
		virtual void generate_mipmaps() override;
		virtual void set_filtering(texture_header::FILTERING min, texture_header::FILTERING mag) override;
		virtual void set_wrapping(texture_header::WRAPPING wrap_s, 
								texture_header::WRAPPING wrap_t,
								texture_header::WRAPPING wrap_r = texture_header::WRAPPING::CLAMP_TO_EDGE) override;

		virtual void update_data(const void* data,
							   uint32_t x_offset,
							   uint32_t y_offset,
							   uint32_t z_offset,
							   uint32_t width,
							   uint32_t height,
							   uint32_t depth,
							   uint32_t level = 0,
							   uint32_t layer = 0,
							   uint32_t face = 0) override;

		// Methods for framebuffer operations
		GLenum get_attachment_type() const;
		GLuint get_id() const { return id; }
		// Identity in a framebuffer cache key: renderbuffers and textures have separate name spaces.
		std::uint64_t framebuffer_object() const
		{
			return static_cast<std::uint64_t>(id) | (header.type == TEXTURE_TYPE::RENDERBUFFER ? (1ull << 32) : 0ull);
		}

	private:
		// Helper methods for texture creation and setup
		void create_texture();
		void set_storage();

		std::shared_ptr<framebuffer_registry> m_framebuffers;
		GLuint id = 0;
		GLenum target = GL_TEXTURE_2D; // OpenGL texture target type
	};
}