#pragma once
#include <snk/render/rnd_render_pass_interface.h>
#include <snk/driver/rnd_vertex_array_interface.h>
#include <snk/driver/rnd_buffer_interface.h>
#include <snk/driver/rnd_shader_interface.h>
#include <memory>

namespace res { class resource_system; }

namespace rnd {
class debug_pass : public render_pass_interface {
public:
	explicit debug_pass(res::resource_system& res_sys);

	void execute(render_context& context, driver::driver_interface& drv) override;

private:
	void ensure_resources(driver::driver_interface& drv);

	res::resource_system& m_res_sys;

	std::unique_ptr<driver::vertex_array_interface> m_vao;
	std::shared_ptr<driver::buffer_interface> m_vertex_buffer;
	std::shared_ptr<driver::buffer_interface> m_index_buffer;
	std::unique_ptr<driver::shader_interface> m_shader;
	std::size_t m_vb_capacity = 0;
	std::size_t m_ib_capacity = 0;
};
} // namespace rnd
