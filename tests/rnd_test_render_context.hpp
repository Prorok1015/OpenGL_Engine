#pragma once
// A render_context for tests that run a pass alone: it serves the packets it is given and forwards the object
// upload and the binds to a per_draw_data, and has nothing else to offer (a pass that asks for a texture, a
// program or a vertex array gets none).
#include <snk/render/rnd_material_groups.h>
#include <snk/render/rnd_material_instance_cache.h>
#include <snk/render/rnd_per_draw_data.h>
#include <snk/render/rnd_render_context.h>
#include <snk/render/skinning/rnd_skinning_manager.h>
#include <memory_resource>
#include <optional>
#include <span>

namespace test_util {

class test_render_context final : public rnd::render_context
{
public:
	explicit test_render_context(rnd::per_draw_data* draw_data = nullptr, rnd::material_groups* materials = nullptr,
		rnd::material_instance_cache* instances = nullptr)
		: m_draw_data(draw_data), m_materials(materials), m_instances(instances)
	{}

	// The packets the passes see; nullptr leaves the frame without any.
	void use_packets(std::pmr::vector<rnd::render_packet_t>* packets) { m_packets = packets; }
	void use_skins(const rnd::skinning_manager* skins) { m_skins = skins; }

	// From now on the passes see only `group` (one render priority, as render_system::render_frame hands them out);
	// use_all_packets() gives back what use_packets set.
	void use_group(std::span<rnd::render_packet_t> group) { m_group = group; }
	void use_all_packets() { m_group.reset(); }

	std::span<rnd::render_packet_t> packets() override
	{
		if (m_group) {
			return *m_group;
		}
		return m_packets ? std::span<rnd::render_packet_t>{ *m_packets } : std::span<rnd::render_packet_t>{};
	}

	rnd::driver::texture_interface* find_texture(const res::tag&) const override { return nullptr; }
	rnd::driver::texture_interface* ensure_render_target(const res::tag&, glm::ivec2, rnd::driver::texture_header::TYPE,
		rnd::driver::texture_usage, const rnd::render_targets::sampling&) override { return nullptr; }
	rnd::driver::vertex_array_interface* geometry(const res::tag&) override { return nullptr; }

	rnd::shader_config_cache& variants() override { return m_variants; }
	rnd::driver::shader_interface* program(rnd::variant_id) override { return nullptr; }
	const rnd::set_layouts& layouts() const override { return m_layouts; }

	void write_view(const rnd::global_params&) override {}
	void write_lights(const rnd::lights_params&) override {}
	void bind_frame() override {}

	const rnd::compiled_material& material(rnd::material_instance_id id) const override
	{
		ASSERT_MSG(m_instances, "this test context was made without a material_instance_cache");
		return m_instances->get(id);
	}

	void bind_material(rnd::material_instance_id id) override
	{
		if (m_materials && m_instances) {
			m_materials->bind(m_instances->get(id));
		}
	}

	void upload_objects(std::span<const rnd::object_data> objects, std::span<const glm::mat4> bones) override
	{
		if (m_draw_data) {
			m_draw_data->upload(objects, bones);
		}
	}

	void bind_object(std::uint32_t index, const res::tag& skin) override
	{
		if (m_draw_data) {
			m_draw_data->bind_object(index, skin, m_skins);
		}
	}

private:
	rnd::per_draw_data* m_draw_data;
	rnd::material_groups* m_materials;
	rnd::material_instance_cache* m_instances;
	std::pmr::vector<rnd::render_packet_t>* m_packets = nullptr;
	std::optional<std::span<rnd::render_packet_t>> m_group;
	const rnd::skinning_manager* m_skins = nullptr;
	rnd::shader_config_cache m_variants;
	rnd::set_layouts m_layouts;
};

}
