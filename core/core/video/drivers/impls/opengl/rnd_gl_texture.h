#pragma once
#include <rnd_texture_interface.h>
#include <glad/glad.h>

namespace rnd::driver::gl
{
	class texture : public rnd::driver::texture_interface
	{
	public:
		// Constructor takes texture header
		explicit texture(const texture_header& hdr);
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

	private:
		// Helper methods for texture creation and setup
		void create_texture();
		void set_storage();

		GLuint id = 0;
		GLenum target = GL_TEXTURE_2D; // OpenGL texture target type
	};
}