#pragma once
#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>
#include <unordered_map>
#include <snk/driver/rnd_bind_group.h>
#include <snk/driver/rnd_pipeline.h>
#include <snk/driver/rnd_render_pass.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace rnd::driver
{
	struct texture_header;
	class shader_interface;
	class texture_interface;
	class swapchain_interface;
	class vertex_array_interface;
	class buffer_interface;

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

		// A frame is what lies between two begin_frame calls: per_frame buffers rotate to their next copy and
		// the transient bind groups of the frame before are gone. The renderer calls it once, before its passes.
		virtual void begin_frame() = 0;

		// The binding numbers the backend gives (set, slot, type). The shader preprocessor and the validation use it.
		virtual const binding_table& bindings() const = 0;
		// Alignment of the offset of a uniform buffer range (GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT): dynamic offsets
		// and the stride of per-draw data have to be multiples of it.
		virtual std::size_t uniform_offset_alignment() const = 0;

		// GPU buffers by handle (uniform and storage buffers; vertex and index data still go through create_buffer()).
		virtual buffer_handle create_buffer(const buffer_desc& desc) = 0;
		virtual void destroy_buffer(buffer_handle buffer) = 0;
		// Writes `data` at `offset`. A per_frame buffer takes the write into the copy of the current frame.
		virtual void update_buffer(buffer_handle buffer, std::size_t offset, std::span<const std::byte> data) = 0;

		// The same entries always give the same layout handle.
		virtual bind_group_layout_handle create_bind_group_layout(const bind_group_layout_desc& desc) = 0;
		// A persistent group lives until destroy_bind_group; a transient one until the next begin_frame, and is
		// not destroyed by the caller. Buffers and textures a group names must outlive its use.
		virtual bind_group_handle create_bind_group(const bind_group_desc& desc, lifetime life = lifetime::persistent) = 0;
		virtual void destroy_bind_group(bind_group_handle group) = 0;
		// Makes `group` the one the draws that follow see at `set`, inside a render pass. `dynamic_offsets` has
		// one entry per dynamic binding of the group's layout, each a multiple of uniform_offset_alignment().
		// Bindings do not survive the render pass.
		virtual void set_bind_group(std::uint32_t set, bind_group_handle group, std::span<const std::uint32_t> dynamic_offsets = {}) = 0;

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