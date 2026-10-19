#pragma once
#include "rnd_null_objects.h"
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_pipeline_cache.h>
#include <snk/common/snk_api.h>

#include <array>

namespace rnd::driver::null
{
	// The null backend derives nothing from a pipeline description.
	struct pipeline_record {};

	// Runs the whole driver_interface without a GPU: objects keep what tests inspect, calls are counted.
	class SNK_API driver final : public rnd::driver::driver_interface
	{
	public:
		struct call_counts
		{
			std::size_t draws = 0;
			std::size_t pipeline_binds = 0;
			std::size_t state_cache_invalidations = 0;
			std::size_t render_pass_begins = 0;
			std::size_t render_pass_ends = 0;
		};

		driver();
		~driver() override = default;

		void begin_render_pass(const render_pass_desc& desc) override;
		void end_render_pass() override;

		void set_viewport(glm::ivec4 rect) override { m_viewport = rect; }
		void set_line_size(float size) override { m_line_size = size; }
		void draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, unsigned int count, unsigned int offset = 0) override;
		void draw_indices(const vertex_array_interface* va, unsigned int count, unsigned int base_vertex, unsigned int base_index) override;

		std::unique_ptr<shader_interface> create_shader(const std::vector<shader_header>& headers) override;
		std::unique_ptr<texture_interface> create_texture(const texture_header& header) override;
		std::unique_ptr<vertex_array_interface> create_vertex_array() override;
		std::unique_ptr<buffer_interface> create_buffer() override;
		std::unique_ptr<uniform_buffer_interface> create_uniform_buffer(std::size_t size, std::size_t binding) override;
		std::unique_ptr<ssbo_buffer_interface> create_ssbo_buffer(std::size_t size, std::size_t binding) override;

		pipeline_handle create_pipeline(const pipeline_desc& desc) override;
		void bind_pipeline(pipeline_handle pipeline) override;
		const pipeline_desc& pipeline_info(pipeline_handle pipeline) const override;
		void invalidate_state_cache() override { ++m_calls.state_cache_invalidations; }

		const object_stats& stats() const { return *m_stats; }
		const call_counts& calls() const { return m_calls; }
		const glm::ivec4& viewport() const { return m_viewport; }
		float line_size() const { return m_line_size; }
		// Pipelines built (misses) and found again (hits) so far.
		const pipeline_cache<pipeline_record>& pipelines() const { return m_pipelines; }
		pipeline_handle bound_pipeline() const { return m_bound; }
		// Draws per topology, to tell a line pass from a triangle pass.
		std::size_t draws_with(RENDER_MODE topology) const { return m_draws_by_topology[static_cast<std::size_t>(topology)]; }
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
		pipeline_cache<pipeline_record> m_pipelines;
		pipeline_handle m_bound;
		std::array<std::size_t, 5> m_draws_by_topology{};
	};
}
