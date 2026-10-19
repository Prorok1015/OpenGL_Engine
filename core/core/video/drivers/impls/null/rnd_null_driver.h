#pragma once
#include "rnd_null_objects.h"
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_pipeline_cache.h>
#include <snk/driver/rnd_resource_tables.h>
#include <snk/driver/rnd_texture_pool.h>
#include <snk/common/snk_api.h>

#include <array>
#include <optional>
#include <span>
#include <vector>

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
			std::size_t frames_begun = 0;
			std::size_t buffers_created = 0;
			std::size_t buffer_updates = 0;
			// Persistent groups cost a driver object each; transient ones only live for the frame.
			std::size_t bind_groups_created = 0;
			std::size_t transient_bind_groups_created = 0;
			std::size_t bind_group_binds = 0;
		};

		// Per set, the draws that had a bind group bound there.
		using draws_per_set = std::array<std::size_t, max_bind_sets>;

		// One set_bind_group call, kept when record_bindings(true) was asked for.
		struct bind_record
		{
			std::uint64_t frame = 0;
			std::uint32_t frame_slot = 0;
			std::uint32_t set = 0;
			bind_group_handle group;
			std::vector<std::uint32_t> dynamic_offsets;
		};

		// What is bound at a set right now (cleared when a render pass begins).
		struct bound_set
		{
			bind_group_handle group;
			std::vector<std::uint32_t> dynamic_offsets;
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
		std::unique_ptr<vertex_array_interface> create_vertex_array() override;
		std::unique_ptr<buffer_interface> create_buffer() override;

		void begin_frame() override;
		const binding_table& bindings() const override { return per_type_binding_table; }
		// The largest alignment the GL drivers report, so what the null backend accepts any of them does.
		std::size_t uniform_offset_alignment() const override { return 256; }
		std::size_t storage_offset_alignment() const override { return 256; }

		buffer_handle create_buffer(const buffer_desc& desc) override;
		void destroy_buffer(buffer_handle buffer) override;
		void update_buffer(buffer_handle buffer, std::size_t offset, std::span<const std::byte> data) override;
		bind_group_layout_handle create_bind_group_layout(const bind_group_layout_desc& desc) override;
		bind_group_handle create_bind_group(const bind_group_desc& desc, lifetime life = lifetime::persistent) override;
		void destroy_bind_group(bind_group_handle group) override;
		void set_bind_group(std::uint32_t set, bind_group_handle group, std::span<const std::uint32_t> dynamic_offsets = {}) override;

		pipeline_handle create_pipeline(const pipeline_desc& desc) override;
		void bind_pipeline(pipeline_handle pipeline) override;
		const pipeline_desc& pipeline_info(pipeline_handle pipeline) const override;
		void invalidate_state_cache() override { ++m_calls.state_cache_invalidations; }
		// A stand-in for the pixels: pixel (x, y) of the texture is {x, y, 0, 255} modulo 256, so a test can tell
		// which part of the texture came back.
		std::vector<std::byte> read_texture(texture_handle texture, glm::ivec4 rect) override;
		texture_handle create_texture(const texture_header& header) override;
		// Immediate: the null backend has no GPU to wait for.
		void destroy(texture_handle texture) override;
		texture_interface* resolve(texture_handle texture) const override { return m_textures.find(texture); }
		const texture_header& info(texture_handle texture) const override { return m_textures.info(texture); }
		bool is_alive(texture_handle texture) const override { return m_textures.find(texture) != nullptr; }

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

		// Frames begun so far and the copy of per_frame buffers the current one writes and binds.
		std::uint64_t frame() const { return m_frame; }
		std::uint32_t frame_slot() const { return static_cast<std::uint32_t>(m_frame % frames_in_flight); }

		const buffer_desc* buffer_info(buffer_handle buffer) const;
		// The bytes of a buffer; of a per_frame buffer the copy `copy` (default the current frame's). Null for a dead handle.
		const std::vector<std::byte>* buffer_bytes(buffer_handle buffer, std::optional<std::uint32_t> copy = std::nullopt) const;
		std::size_t live_buffers() const { return m_buffers.live(); }
		// The textures that are alive. A texture is gone with destroy() (or the driver).
		std::size_t live_textures() const { return m_textures.live(); }
		// The object behind a handle: null for a handle that names no live texture.
		texture* texture_of(texture_handle handle) const { return static_cast<texture*>(m_textures.find(handle)); }
		const bind_group_layout_desc* layout_info(bind_group_layout_handle layout) const { return m_layouts.find(layout); }
		std::size_t layout_count() const { return m_layouts.size(); }
		// Null for a handle that names no live group (a transient one is gone after its frame).
		const bind_group_desc* group_info(bind_group_handle group) const;
		std::size_t live_bind_groups() const { return m_groups.live(); }
		const bound_set& bound(std::uint32_t set) const { return m_bound_sets[set]; }

		// Keeps every set_bind_group call in bind_log(); off by default, a long run would collect them all.
		void record_bindings(bool on) { m_record_bindings = on; }
		const std::vector<bind_record>& bind_log() const { return m_bind_log; }
		const draws_per_set& draws_with_set() const { return m_draws_with_set; }

		// Keeps the sources of the programs created from now on in shader_sources(); off by default.
		void record_shader_sources(bool on) { m_record_shader_sources = on; }
		const std::vector<std::vector<shader_header>>& shader_sources() const { return m_shader_sources; }

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

		struct null_buffer
		{
			buffer_desc desc;
			// One copy, or frames_in_flight for a per_frame buffer.
			std::vector<std::vector<std::byte>> copies;
		};
		struct null_group
		{
			bind_group_desc desc;
			lifetime life = lifetime::persistent;
		};

		std::uint64_t m_frame = 0;
		texture_pool m_textures;
		bind_group_layout_cache m_layouts;
		slot_table<buffer_tag, null_buffer> m_buffers;
		slot_table<bind_group_tag, null_group> m_groups;
		std::vector<bind_group_handle> m_transient_groups;
		std::array<bound_set, max_bind_sets> m_bound_sets;
		bool m_record_bindings = false;
		std::vector<bind_record> m_bind_log;
		draws_per_set m_draws_with_set{};
		bool m_record_shader_sources = false;
		std::vector<std::vector<shader_header>> m_shader_sources;

		void count_draw();
	};
}
