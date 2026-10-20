#pragma once
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_pipeline_cache.h>
#include <snk/driver/rnd_resource_tables.h>
#include <snk/driver/rnd_texture_backend.h>
#include <snk/driver/rnd_texture_pool.h>
#include <optional>
#include <array>
#include <memory>
#include <vector>
#include <glad/glad.h>

namespace rnd::driver::gl
{
	class framebuffer_registry;
	class texture;

	class driver : public rnd::driver::driver_interface, public rnd::driver::texture_object_access
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
		virtual std::unique_ptr<vertex_array_interface> create_vertex_array() override;
		virtual std::unique_ptr<buffer_interface> create_buffer() override;

		virtual void begin_frame() override;
		virtual const binding_table& bindings() const override { return per_type_binding_table; }
		virtual std::size_t uniform_offset_alignment() const override;
		virtual std::size_t storage_offset_alignment() const override;

		virtual buffer_handle create_buffer(const buffer_desc& desc) override;
		virtual void destroy_buffer(buffer_handle buffer) override;
		virtual void update_buffer(buffer_handle buffer, std::size_t offset, std::span<const std::byte> data) override;
		virtual bind_group_layout_handle create_bind_group_layout(const bind_group_layout_desc& desc) override;
		virtual bind_group_handle create_bind_group(const bind_group_desc& desc, lifetime life = lifetime::persistent) override;
		virtual void destroy_bind_group(bind_group_handle group) override;
		virtual void set_bind_group(std::uint32_t set, bind_group_handle group, std::span<const std::uint32_t> dynamic_offsets = {}) override;

		virtual pipeline_handle create_pipeline(const pipeline_desc& desc) override;
		virtual void bind_pipeline(pipeline_handle pipeline) override;
		virtual const pipeline_desc& pipeline_info(pipeline_handle pipeline) const override;
		virtual void invalidate_state_cache() override;
		virtual std::vector<std::byte> read_texture(texture_handle texture, glm::ivec4 rect) override;
		virtual texture_handle create_texture(const texture_header& header) override;
		// GL frees the texture at once: the driver keeps the GPU work in order, so nothing needs to be waited for.
		virtual void destroy(texture_handle texture) override;
		virtual void update_texture(texture_handle texture, const void* data, const texture_region& region) override;
		virtual std::uint64_t native_texture_name(texture_handle texture) const override;
		virtual texture_interface* texture_object(texture_handle texture) const override { return m_textures.find(texture); }
		virtual const texture_header& info(texture_handle texture) const override { return m_textures.info(texture); }
		virtual bool is_alive(texture_handle texture) const override { return m_textures.find(texture) != nullptr; }

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
			std::optional<GLenum> polygon;
		};

		// A buffer's GL objects: one, or frames_in_flight for a per_frame buffer (the copy of the frame is bound).
		struct gl_buffer
		{
			buffer_desc desc;
			std::array<GLuint, frames_in_flight> ids{};
			std::uint32_t copies = 1;

			GLuint current(std::uint32_t frame_slot) const { return ids[desc.update == buffer_update::per_frame ? frame_slot : 0]; }
		};

		// A bind group is its description; the layout says how to bind each entry.
		struct gl_group
		{
			bind_group_desc desc;
		};

		// The GL texture behind a handle; null for a handle that names no live texture.
		texture* texture_of(texture_handle handle) const;
		std::uint32_t frame_slot() const { return static_cast<std::uint32_t>(m_frame % frames_in_flight); }
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
		std::uint64_t m_frame = 0;
		mutable std::optional<std::size_t> m_uniform_offset_alignment;
		mutable std::optional<std::size_t> m_storage_offset_alignment;
		texture_pool m_textures;
		bind_group_layout_cache m_layouts;
		slot_table<buffer_tag, gl_buffer> m_buffers;
		slot_table<bind_group_tag, gl_group> m_groups;
		std::vector<bind_group_handle> m_transient_groups;
		// Attachments to invalidate when the pass ends (store_op::dont_care).
		std::array<GLenum, render_pass_desc::max_colors + 1> m_pass_invalidate{};
		std::size_t m_pass_invalidate_count = 0;
	};
}