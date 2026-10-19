#pragma once
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_pipeline_cache.h>
#include <optional>
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
		virtual void draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, unsigned int count, unsigned int offset = 0) override;
		virtual void draw_indices(const vertex_array_interface* va, unsigned int count, unsigned int base_vertex, unsigned int base_index) override;

		virtual std::unique_ptr<shader_interface> create_shader(const std::vector<shader_header>& headers) override;
		virtual std::unique_ptr<texture_interface> create_texture(const texture_header& header) override;
		virtual std::unique_ptr<vertex_array_interface> create_vertex_array() override;
		virtual std::unique_ptr<buffer_interface> create_buffer() override;
		virtual std::unique_ptr<uniform_buffer_interface> create_uniform_buffer(std::size_t size, std::size_t binding) override;
		virtual std::unique_ptr<ssbo_buffer_interface> create_ssbo_buffer(std::size_t size, std::size_t binding) override;

		virtual pipeline_handle create_pipeline(const pipeline_desc& desc) override;
		virtual void bind_pipeline(pipeline_handle pipeline) override;
		virtual const pipeline_desc& pipeline_info(pipeline_handle pipeline) const override;
		virtual void invalidate_state_cache() override;

		// GPU timer queries
		virtual bool supports_timer_queries() const override;
		virtual uint32_t create_timer_query() override;
		virtual void destroy_timer_query(uint32_t id) override;
		virtual void timestamp_query(uint32_t id) override;
		virtual bool is_query_ready(uint32_t id) const override;
		virtual uint64_t get_query_result_ns(uint32_t id) const override;

	private:
		struct gl_pipeline
		{
			GLenum topology = GL_TRIANGLES;
		};

		// What the driver believes the GL state is; an empty field is unknown and gets written on the next bind.
		struct state_cache
		{
			std::optional<GLuint> program;
			std::optional<bool> depth_test;
			std::optional<GLenum> depth_func;
			std::optional<bool> depth_mask;
			std::optional<bool> blend;
			struct blend_funcs
			{
				GLenum src, dst, equation;
				friend bool operator==(const blend_funcs&, const blend_funcs&) = default;
			};
			std::array<std::optional<blend_funcs>, pipeline_desc::max_targets> target_blend;
			std::optional<bool> cull;
			std::optional<GLenum> cull_face;
		};

		void apply_pipeline(const pipeline_desc& desc);
		GLuint framebuffer_for(const render_pass_desc& desc);
		GLuint create_framebuffer(const render_pass_desc& desc);

		glm::ivec4 viewport{ 0 };
		std::shared_ptr<framebuffer_registry> m_framebuffers;
		bool m_in_pass = false;
		GLuint m_pass_fbo = 0;
		bool m_pass_debug_group = false;
		// Depth writes as the last bound pipeline left them: a depth clear is subject to the mask.
		// Unlike the state cache this survives invalidate_state_cache: foreign code does not touch the mask.
		bool m_depth_write = true;
		pipeline_cache<gl_pipeline> m_pipelines;
		const pipeline_cache<gl_pipeline>::entry* m_bound = nullptr;
		state_cache m_state;
		// Attachments to invalidate when the pass ends (store_op::dont_care).
		std::array<GLenum, render_pass_desc::max_colors + 1> m_pass_invalidate{};
		std::size_t m_pass_invalidate_count = 0;
	};
}