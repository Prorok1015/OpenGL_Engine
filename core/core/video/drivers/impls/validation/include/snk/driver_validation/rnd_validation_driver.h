#pragma once
#include <snk/driver_validation/rnd_validation_objects.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_swapchain_interface.h>
#include <snk/driver/rnd_texture_backend.h>
#include <snk/driver/rnd_texture_pool.h>
#include <snk/common/snk_api.h>

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rnd::driver::validation
{
	// Decorator over any driver_interface: checks the command stream against the rules of rnd_validation_validator.h,
	// reports violations (log category "rnd.validate" plus counters) and forwards every call. Calls that would
	// crash the wrapped driver (a dead object, a write past the end of a buffer, a nested or unmatched render
	// pass) are reported and dropped.
	class SNK_API driver final : public rnd::driver::driver_interface, public rnd::driver::texture_object_access
	{
	public:
		explicit driver(std::unique_ptr<driver_interface> inner, limits budget = {});
		driver(std::unique_ptr<driver_interface> inner, std::shared_ptr<validator> state);
		~driver() override = default;

		void begin_render_pass(const render_pass_desc& desc) override;
		void end_render_pass() override;

		void set_viewport(glm::ivec4 rect) override;
		void set_line_size(float size) override { m_inner->set_line_size(size); }
		void draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, unsigned int count, unsigned int offset = 0) override;
		void draw_indices(const vertex_array_interface* va, unsigned int count, unsigned int base_vertex, unsigned int base_index) override;

		std::unique_ptr<shader_interface> create_shader(const std::vector<shader_header>& headers) override;
		std::unique_ptr<vertex_array_interface> create_vertex_array() override;
		std::unique_ptr<buffer_interface> create_buffer() override;

		void begin_frame() override;
		const binding_table& bindings() const override { return m_inner->bindings(); }
		std::size_t uniform_offset_alignment() const override { return m_inner->uniform_offset_alignment(); }
		std::size_t storage_offset_alignment() const override { return m_inner->storage_offset_alignment(); }

		buffer_handle create_buffer(const buffer_desc& desc) override;
		void destroy_buffer(buffer_handle buffer) override;
		void update_buffer(buffer_handle buffer, std::size_t offset, std::span<const std::byte> data) override;
		bind_group_layout_handle create_bind_group_layout(const bind_group_layout_desc& desc) override;
		bind_group_handle create_bind_group(const bind_group_desc& desc, lifetime life = lifetime::persistent) override;
		void destroy_bind_group(bind_group_handle group) override;
		void set_bind_group(std::uint32_t set, bind_group_handle group, std::span<const std::uint32_t> dynamic_offsets = {}) override;

		pipeline_handle create_pipeline(const pipeline_desc& desc) override;
		void destroy_pipelines_of(const shader_ref& shader) override;
		void bind_pipeline(pipeline_handle pipeline) override;
		const pipeline_desc& pipeline_info(pipeline_handle pipeline) const override;
		void invalidate_state_cache() override { m_inner->invalidate_state_cache(); }
		// Reports (stale_handle or dead_object) and returns nothing for a handle of no live texture; reports
		// (invalid_read) for one that cannot be read back and a rect outside it; otherwise forwards.
		std::vector<std::byte> read_texture(texture_handle texture, glm::ivec4 rect) override;
		// Creates the texture in the wrapped driver and a wrapper for it; the handle is this driver's own (its
		// driver id differs from the wrapped driver's, so the inner handle is not accepted here).
		texture_handle create_texture(const texture_header& header) override;
		// Reports (stale_handle or dead_object) a handle of no live texture; otherwise destroys the wrapper and then
		// the wrapped driver's texture.
		void destroy(texture_handle texture) override;
		// Reports (stale_handle or dead_object) a handle of no live texture; otherwise writes through the wrapper.
		void update_texture(texture_handle texture, const void* data, const texture_region& region) override;
		// The name the wrapped driver gives the texture; 0 for a handle of no live texture of this driver (silent).
		std::uint64_t native_texture_name(texture_handle texture) const override { return m_inner->native_texture_name(inner_handle(texture)); }
		// The wrapper behind the handle (its native() is the wrapped driver's object); silent, null for no live texture.
		texture_interface* texture_object(texture_handle texture) const override { return m_textures.find(texture); }
		// The handle the wrapped driver gave the texture a wrapper stands for; null for a handle of no live texture.
		texture_handle inner_handle(texture_handle handle) const;
		const texture_header& info(texture_handle texture) const override { return m_textures.info(texture); }
		bool is_alive(texture_handle texture) const override { return m_textures.find(texture) != nullptr; }

		bool supports_timer_queries() const override { return m_inner->supports_timer_queries(); }
		uint32_t create_timer_query() override { return m_inner->create_timer_query(); }
		void destroy_timer_query(uint32_t id) override { m_inner->destroy_timer_query(id); }
		void timestamp_query(uint32_t id) override { m_inner->timestamp_query(id); }
		bool is_query_ready(uint32_t id) const override { return m_inner->is_query_ready(id); }
		uint64_t get_query_result_ns(uint32_t id) const override { return m_inner->get_query_result_ns(id); }

		driver_interface& inner() { return *m_inner; }
		validator& state() { return *m_state; }
		const validator& state() const { return *m_state; }

	private:
		void check_texture_usage(const texture_header& header);
		// The wrapper behind a handle. A handle that names no live texture is reported (stale_handle for one whose
		// texture was destroyed, dead_object for one this driver never issued) and gives null.
		validation::texture* live_texture(texture_handle handle, const char* what);
		// Reports and returns false for a shader the pipeline names that is gone.
		bool shader_alive(const shader_ref& ref);
		// False when the pass cannot be forwarded; otherwise `inner_desc` is `desc` with the wrapped driver's textures.
		bool check_render_pass(const render_pass_desc& desc, render_pass_desc& inner_desc, target_info& target);
		// Null when the draw cannot be forwarded.
		const vertex_array* check_draw(const vertex_array_interface* va, unsigned int count, std::size_t base_index);
		// Every set the bound pipeline names has a group of the pipeline's layout bound.
		void check_pipeline_sets(std::size_t pipeline_index);

		template <class Tag>
		static std::uint64_t key_of(handle<Tag> h) { return static_cast<std::uint64_t>(h.index) | static_cast<std::uint64_t>(h.gen) << 32; }

		// Reports and returns false when the buffer range an entry names is not inside a live buffer of the right usage.
		bool check_buffer_entry(const binding_res& entry, const binding_decl& decl);

		struct known_pipeline
		{
			pipeline_handle handle;
			pipeline_desc desc;	// as the caller wrote it: the shader is the wrapper, not the wrapped driver's
		};

		struct known_group
		{
			bind_group_layout_handle layout;
			bind_group_desc desc;	// as the caller wrote it: the textures are the wrappers
			lifetime life = lifetime::persistent;
			std::uint64_t epoch = 0;	// the frame a transient group belongs to
		};

		// A transient group that ended with its frame, remembered for a few frames to tell its late use from a wrong handle.
		struct expired_group
		{
			std::uint64_t key;
			std::uint64_t epoch;
		};

		std::unique_ptr<driver_interface> m_inner;
		// The same object as m_inner, null when it gives no access (create_texture then fails): how the wrappers get the wrapped driver's texture objects.
		const texture_object_access* m_inner_textures = nullptr;
		std::shared_ptr<validator> m_state;
		// The wrappers create_texture handed out, by the handles this driver issued (the wrapped driver has its own).
		texture_pool m_textures;
		std::vector<known_pipeline> m_pipelines;	// by the index of the handle; a dropped pipeline leaves a null handle
		std::unordered_map<std::uint64_t, buffer_desc> m_buffers;
		std::unordered_map<std::uint64_t, bind_group_layout_desc> m_layouts;
		std::unordered_map<std::uint64_t, known_group> m_groups;
		std::vector<expired_group> m_expired_groups;
		// What the draws of the render pass in progress see; a pass starts with nothing bound.
		std::array<bind_group_handle, max_bind_sets> m_bound_sets;
		std::size_t m_bound_pipeline = 0;	// index (1-based) into m_pipelines, 0 = none
	};

	// Counts presents for the driver it creates: a frame ends at present().
	class SNK_API swapchain final : public rnd::driver::swapchain_interface
	{
	public:
		explicit swapchain(std::unique_ptr<swapchain_interface> inner, limits budget = {});

		std::unique_ptr<rnd::driver::driver_interface> create_driver() override;
		void present() override;
		void set_vsync(bool enabled) override { m_inner->set_vsync(enabled); }

		swapchain_interface& inner() { return *m_inner; }
		validator& state() { return *m_state; }

	private:
		std::unique_ptr<swapchain_interface> m_inner;
		std::shared_ptr<validator> m_state;
	};
}
