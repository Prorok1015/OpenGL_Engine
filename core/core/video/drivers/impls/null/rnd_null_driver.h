#pragma once
#include "rnd_null_objects.h"
#include <snk/driver/rnd_driver_interface.h>
#include <snk/common/snk_api.h>

namespace rnd::driver::null
{
	// Runs the whole driver_interface without a GPU: objects keep what tests inspect, calls are counted.
	class SNK_API driver final : public rnd::driver::driver_interface
	{
	public:
		struct call_counts
		{
			std::size_t draws = 0;
			std::size_t clears = 0;
			std::size_t push_frame_buffers = 0;
			std::size_t pop_frame_buffers = 0;
			std::size_t render_target_sets = 0;
			std::size_t render_state_sets = 0;
		};

		driver();
		~driver() override = default;

		void push_frame_buffer() override;
		void pop_frame_buffer() override;
		void set_render_target(texture_interface* color, texture_interface* depth_stencil = nullptr) override;
		void set_render_targets(std::vector<texture_interface*> colors, texture_interface* depth_stencil = nullptr) override;

		void set_viewport(glm::ivec4 rect) override { m_viewport = rect; }
		void set_scissor(glm::ivec4 rect) override { m_scissor = rect; }
		void clear(clear_flags flags) override;
		void clear(clear_flags flags, glm::vec4 color) override;
		void clear(clear_flags flags, const std::vector<glm::vec4>& colors) override;
		void set_line_size(float size) override { m_line_size = size; }
		void draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, RENDER_MODE render_mode, unsigned int count, unsigned int offset = 0) override;
		void draw_indices(const vertex_array_interface* va, RENDER_MODE rm, unsigned int count, unsigned int base_vertex, unsigned int base_index) override;

		std::unique_ptr<shader_interface> create_shader(const std::vector<shader_header>& headers) override;
		std::unique_ptr<texture_interface> create_texture(const texture_header& header) override;
		std::unique_ptr<vertex_array_interface> create_vertex_array() override;
		std::unique_ptr<buffer_interface> create_buffer() override;
		std::unique_ptr<uniform_buffer_interface> create_uniform_buffer(std::size_t size, std::size_t binding) override;
		std::unique_ptr<ssbo_buffer_interface> create_ssbo_buffer(std::size_t size, std::size_t binding) override;

		void set_render_state(const render_state& state) override;

		const object_stats& stats() const { return *m_stats; }
		const call_counts& calls() const { return m_calls; }
		// 1 is the backbuffer; every push_frame_buffer adds one.
		std::size_t frame_buffer_depth() const { return m_frame_buffer_depth; }
		const glm::ivec4& viewport() const { return m_viewport; }
		const glm::ivec4& scissor() const { return m_scissor; }
		float line_size() const { return m_line_size; }
		const render_state& current_render_state() const { return m_render_state; }

	private:
		std::shared_ptr<object_stats> m_stats;
		call_counts m_calls;
		std::size_t m_frame_buffer_depth = 1;
		glm::ivec4 m_viewport{ 0 };
		glm::ivec4 m_scissor{ 0 };
		float m_line_size = 1.f;
		render_state m_render_state;
	};
}
