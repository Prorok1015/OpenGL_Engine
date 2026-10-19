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
			std::size_t render_state_sets = 0;
			std::size_t render_pass_begins = 0;
			std::size_t render_pass_ends = 0;
		};

		driver();
		~driver() override = default;

		void begin_render_pass(const render_pass_desc& desc) override;
		void end_render_pass() override;

		void set_viewport(glm::ivec4 rect) override { m_viewport = rect; }
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
		const glm::ivec4& viewport() const { return m_viewport; }
		float line_size() const { return m_line_size; }
		const render_state& current_render_state() const { return m_render_state; }
		bool in_render_pass() const { return m_in_render_pass; }
		// The pass begun last, still valid after it ended.
		const render_pass_desc& last_render_pass() const { return m_last_pass; }

	private:
		std::shared_ptr<object_stats> m_stats;
		call_counts m_calls;
		bool m_in_render_pass = false;
		render_pass_desc m_last_pass;
		std::uint32_t m_next_framebuffer = 0;
		glm::ivec4 m_viewport{ 0 };
		float m_line_size = 1.f;
		render_state m_render_state;
	};
}
