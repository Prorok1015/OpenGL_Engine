#pragma once
#include <rnd_driver_interface.h>
#include <stack>
#include <glad/glad.h>

namespace rnd::driver::gl
{
	class driver : public rnd::driver::driver_interface
	{
	public:
		driver();
		virtual ~driver() override;

		virtual void push_frame_buffer() override;
		virtual void pop_frame_buffer() override;
		virtual void set_render_target(texture_interface* color, texture_interface* depth_stencil = nullptr) override;
		virtual void set_render_targets(std::vector<texture_interface*> colors, texture_interface* depth_stencil = nullptr) override;

		virtual void set_viewport(glm::ivec4 rect) override;
		virtual void set_clear_color(glm::vec4 color) override;
		virtual void clear(clear_flags flags) override;
		virtual void clear(clear_flags flags, glm::vec4 color) override;
		virtual void clear(clear_flags flags, const std::vector<glm::vec4>& colors) override;
		virtual void set_activate_texture(int idx) override;
		virtual void set_line_size(float size) override;
		virtual void set_point_size(float size) override;
		virtual void draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, RENDER_MODE render_mode, unsigned int count, unsigned int offset = 0) override;
		virtual void draw_instanced_indices(const std::unique_ptr<vertex_array_interface>& vertices, RENDER_MODE render_mode, unsigned int count, unsigned int instance_count, unsigned int offset = 0) override;
		virtual void draw_indices(const vertex_array_interface* va, RENDER_MODE rm, unsigned int count, unsigned int base_vertex, unsigned int base_index) override;

		virtual std::unique_ptr<shader_interface> create_shader(const std::vector<shader_header>& headers) override;
		virtual std::unique_ptr<texture_interface> create_texture(const texture_header& header) override;
		virtual std::unique_ptr<vertex_array_interface> create_vertex_array() override;
		virtual std::unique_ptr<buffer_interface> create_buffer() override;
		virtual std::unique_ptr<uniform_buffer_interface> create_uniform_buffer(std::size_t size, std::size_t binding) override;

		virtual void register_render_state(const render_state& state) override;
		virtual void set_render_state(const render_state& state) override;

	private:
		// Helper methods for framebuffer management
		void attach_frame_buffer();
		void detach_frame_buffer();
		void check_frame_buffer();

		glm::ivec4 viewport{ 0 };
		std::stack<std::pair<GLuint, GLuint>> framebuffers;
		render_state_cache state_cache;
	};
}