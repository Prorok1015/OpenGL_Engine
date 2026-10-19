#pragma once
#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>
#include <unordered_map>
#include <snk/driver/rnd_pipeline.h>
#include <snk/driver/rnd_render_pass.h>

namespace rnd::driver
{
	struct texture_header;
	class shader_interface;
	class texture_interface;
	class swapchain_interface;
	class vertex_array_interface;
	class uniform_buffer_interface;
	class buffer_interface;
	class ssbo_buffer_interface;

	struct shader_header
	{
		enum class shader_type { VERTEX, FRAGMENT, GEOMETRY };
		std::string title;
		std::string body;
		shader_type type;
	};

	class driver_interface
	{
	public:
		virtual ~driver_interface() {}

		// A pass binds its attachments, applies the load ops and ends with end_render_pass. Passes do not nest.
		virtual void begin_render_pass(const render_pass_desc& desc) = 0;
		virtual void end_render_pass() = 0;

		virtual void set_viewport(glm::ivec4 rect) = 0;
		virtual void set_line_size(float size) = 0;
		// Draws go through the bound pipeline: its program, fixed-function state and topology.
		virtual void draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, unsigned int count, unsigned int offset = 0) = 0;
		virtual void draw_indices(const vertex_array_interface* va, unsigned int count, unsigned int base_vertex, unsigned int base_index) = 0;


		virtual std::unique_ptr<shader_interface> create_shader(const std::vector<shader_header>& headers) = 0;
		virtual std::unique_ptr<texture_interface> create_texture(const texture_header& header) = 0;
		virtual std::unique_ptr<vertex_array_interface> create_vertex_array() = 0;
		virtual std::unique_ptr<buffer_interface> create_buffer() = 0;
		virtual std::unique_ptr<uniform_buffer_interface> create_uniform_buffer(std::size_t size, std::size_t binding) = 0;
		virtual std::unique_ptr<ssbo_buffer_interface> create_ssbo_buffer(std::size_t size, std::size_t binding) = 0;

		// The same description always gives the same handle. The pipeline is immutable and lives as long as the driver.
		virtual pipeline_handle create_pipeline(const pipeline_desc& desc) = 0;
		// Makes the pipeline current for the draws that follow, inside a render pass whose formats it declares.
		// The state of a previous pipeline is not inherited: everything a draw depends on is in the pipeline.
		virtual void bind_pipeline(pipeline_handle pipeline) = 0;
		// The description a pipeline was created from; an empty one for a handle that names no pipeline.
		virtual const pipeline_desc& pipeline_info(pipeline_handle pipeline) const = 0;
		// Forget what the driver believes the API state is, after foreign code (ImGui) touched it.
		virtual void invalidate_state_cache() = 0;

		// GPU timer queries for profiling
		virtual bool supports_timer_queries() const { return false; }
		virtual uint32_t create_timer_query() { return 0; }
		virtual void destroy_timer_query(uint32_t /*id*/) {}
		virtual void timestamp_query(uint32_t /*id*/) {}
		virtual bool is_query_ready(uint32_t /*id*/) const { return false; }
		virtual uint64_t get_query_result_ns(uint32_t /*id*/) const { return 0; }
	};
}