#pragma once
#include <snk/driver/rnd_driver_interface.h>
#include <array>
#include <memory>
#include <glad/glad.h>

namespace rnd::driver::gl
{
	class framebuffer_registry;

	class driver : public rnd::driver::driver_interface
	{
	public:
		driver();
		virtual ~driver() override;

		virtual void begin_render_pass(const render_pass_desc& desc) override;
		virtual void end_render_pass() override;

		virtual void set_viewport(glm::ivec4 rect) override;
		virtual void set_line_size(float size) override;
		virtual void draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, RENDER_MODE render_mode, unsigned int count, unsigned int offset = 0) override;
		virtual void draw_indices(const vertex_array_interface* va, RENDER_MODE rm, unsigned int count, unsigned int base_vertex, unsigned int base_index) override;

		virtual std::unique_ptr<shader_interface> create_shader(const std::vector<shader_header>& headers) override;
		virtual std::unique_ptr<texture_interface> create_texture(const texture_header& header) override;
		virtual std::unique_ptr<vertex_array_interface> create_vertex_array() override;
		virtual std::unique_ptr<buffer_interface> create_buffer() override;
		virtual std::unique_ptr<uniform_buffer_interface> create_uniform_buffer(std::size_t size, std::size_t binding) override;
		virtual std::unique_ptr<ssbo_buffer_interface> create_ssbo_buffer(std::size_t size, std::size_t binding) override;

		virtual void set_render_state(const render_state& state) override;

		// GPU timer queries
		virtual bool supports_timer_queries() const override;
		virtual uint32_t create_timer_query() override;
		virtual void destroy_timer_query(uint32_t id) override;
		virtual void timestamp_query(uint32_t id) override;
		virtual bool is_query_ready(uint32_t id) const override;
		virtual uint64_t get_query_result_ns(uint32_t id) const override;

	private:
		GLuint framebuffer_for(const render_pass_desc& desc);
		GLuint create_framebuffer(const render_pass_desc& desc);

		glm::ivec4 viewport{ 0 };
		std::shared_ptr<framebuffer_registry> m_framebuffers;
		bool m_in_pass = false;
		GLuint m_pass_fbo = 0;
		bool m_pass_debug_group = false;
		// Depth writes as the last render state left them: a depth clear is subject to the mask.
		bool m_depth_write = true;
		// Attachments to invalidate when the pass ends (store_op::dont_care).
		std::array<GLenum, render_pass_desc::max_colors + 1> m_pass_invalidate{};
		std::size_t m_pass_invalidate_count = 0;
	};
}