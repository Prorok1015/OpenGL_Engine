#pragma once
// A render_context for tests that run the passes over the null driver: every texture, vertex array and program a
// pass asks for exists, so each draw of a packet reaches the driver.
#include "rnd_null_driver.h"
#include <snk/render/rnd_render_context.h>
#include <snk/render/rnd_render_packet.hpp>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <vector>

namespace test_util {

// What a draw of every kind needs from the renderer, served by the null driver.
class drawing_context final : public rnd::render_context
{
public:
	explicit drawing_context(rnd::driver::null::driver& drv)
	{
		m_texture = drv.create_texture(rnd::driver::texture_header{});
		m_vertex_array = drv.create_vertex_array();
		m_program = drv.create_shader({
			{ "a.vert", "#version 450 core\nvoid main() {}\n", rnd::driver::shader_header::shader_type::VERTEX },
			{ "a.frag", "#version 450 core\nvoid main() {}\n", rnd::driver::shader_header::shader_type::FRAGMENT },
		});
		const auto layout = drv.create_bind_group_layout({});
		m_layouts = { layout, layout, layout, layout, layout };

		rnd::shader_config config;
		m_material.base = m_material.opaque = m_variants.intern(config.cdata);
		m_material.runtime.samplers.push_back(rnd::texture_id{ 1 });
	}

	void use_packets(std::pmr::vector<rnd::render_packet_t>* packets) { m_packets = packets; }

	std::span<rnd::render_packet_t> packets() override { return m_packets ? std::span<rnd::render_packet_t>{ *m_packets } : std::span<rnd::render_packet_t>{}; }
	rnd::driver::texture_handle find_texture(const res::tag&) const override { return m_texture; }
	rnd::driver::texture_handle texture(rnd::texture_id) const override { return m_texture; }
	rnd::driver::texture_handle ensure_render_target(const res::tag&, glm::ivec2, rnd::driver::texture_header::TYPE,
		rnd::driver::texture_usage, const rnd::render_targets::sampling&) override { return m_texture; }
	rnd::driver::vertex_array_interface* geometry(const res::tag&) override { return m_vertex_array.get(); }
	rnd::shader_config_cache& variants() override { return m_variants; }
	rnd::driver::shader_interface* program(rnd::variant_id) override { return m_program.get(); }
	const rnd::set_layouts& layouts() const override { return m_layouts; }
	void write_view(const rnd::global_params& view) override { m_view_times.push_back(view.time); }
	void write_lights(const rnd::lights_params&) override {}
	void bind_frame() override {}
	const rnd::compiled_material& material(rnd::material_instance_id) const override { return m_material; }
	void bind_material(rnd::material_instance_id) override {}
	void upload_objects(std::span<const rnd::object_data>, std::span<const glm::mat4>) override {}
	void bind_object(std::uint32_t, const res::tag&) override {}

	rnd::variant_id variant() const { return m_material.base; }
	// The `time` of every write_view, in order.
	const std::vector<float>& view_times() const { return m_view_times; }

private:
	std::pmr::vector<rnd::render_packet_t>* m_packets = nullptr;
	rnd::driver::texture_handle m_texture;
	std::unique_ptr<rnd::driver::vertex_array_interface> m_vertex_array;
	std::unique_ptr<rnd::driver::shader_interface> m_program;
	rnd::set_layouts m_layouts;
	rnd::shader_config_cache m_variants;
	rnd::compiled_material m_material;
	std::vector<float> m_view_times;
};

}
