#pragma once
#include "rnd_validation_objects.h"
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_swapchain_interface.h>
#include <snk/common/snk_api.h>

#include <memory>

namespace rnd::driver::validation
{
	// Decorator over any driver_interface: checks the command stream against the rules of rnd_validation_validator.h,
	// reports violations (log category "rnd.validate" plus counters) and forwards every call. Calls that would
	// crash the wrapped driver (a dead object, a write past the end of a buffer, a nested or unmatched render
	// pass) are reported and dropped.
	class SNK_API driver final : public rnd::driver::driver_interface
	{
	public:
		explicit driver(std::unique_ptr<driver_interface> inner, limits budget = {});
		driver(std::unique_ptr<driver_interface> inner, std::shared_ptr<validator> state);
		~driver() override = default;

		void begin_render_pass(const render_pass_desc& desc) override;
		void end_render_pass() override;

		void set_viewport(glm::ivec4 rect) override;
		void set_line_size(float size) override { m_inner->set_line_size(size); }
		void draw_indices(const std::unique_ptr<vertex_array_interface>& vertices, RENDER_MODE render_mode, unsigned int count, unsigned int offset = 0) override;
		void draw_indices(const vertex_array_interface* va, RENDER_MODE rm, unsigned int count, unsigned int base_vertex, unsigned int base_index) override;

		std::unique_ptr<shader_interface> create_shader(const std::vector<shader_header>& headers) override;
		std::unique_ptr<texture_interface> create_texture(const texture_header& header) override;
		std::unique_ptr<vertex_array_interface> create_vertex_array() override;
		std::unique_ptr<buffer_interface> create_buffer() override;
		std::unique_ptr<uniform_buffer_interface> create_uniform_buffer(std::size_t size, std::size_t binding) override;
		std::unique_ptr<ssbo_buffer_interface> create_ssbo_buffer(std::size_t size, std::size_t binding) override;

		void set_render_state(const render_state& state) override { m_inner->set_render_state(state); }

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
		// False when the pass cannot be forwarded; otherwise `inner_desc` is `desc` with the wrapped driver's textures.
		bool check_render_pass(const render_pass_desc& desc, render_pass_desc& inner_desc, target_info& target);
		// Null when the draw cannot be forwarded.
		const vertex_array* check_draw(const vertex_array_interface* va, unsigned int count, std::size_t base_index);

		std::unique_ptr<driver_interface> m_inner;
		std::shared_ptr<validator> m_state;
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
