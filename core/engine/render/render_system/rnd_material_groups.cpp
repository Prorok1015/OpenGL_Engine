#include <snk/render/rnd_material_groups.h>
#include <snk/render/shader/rnd_material_params.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {
	std::unique_ptr<rnd::driver::texture_interface> make_placeholder(rnd::driver::driver_interface& drv, const char* name, std::array<unsigned char, 4> texel)
	{
		rnd::driver::texture_header header;
		header.name = name;
		header.data.extent.width = 1;
		header.data.extent.height = 1;
		header.data.format = rnd::driver::texture_header::TYPE::RGBA8;
		header.data.initial_data = texel.data();
		header.min = rnd::driver::texture_header::FILTERING::NEAREST;
		header.mag = rnd::driver::texture_header::FILTERING::NEAREST;
		return drv.create_texture(header);
	}
}

rnd::material_groups::material_groups(driver::driver_interface& drv)
	: m_drv(drv)
	, m_layout(drv.create_bind_group_layout(material_layout_desc()))
{
	// What a map the material lacks reads as: white colour, no specular, a normal straight out of the surface.
	m_placeholders[bindings::diffuse_slot] = make_placeholder(drv, "material diffuse placeholder", { 255, 255, 255, 255 });
	m_placeholders[bindings::specular_slot] = make_placeholder(drv, "material specular placeholder", { 0, 0, 0, 255 });
	m_placeholders[bindings::normal_slot] = make_placeholder(drv, "material normal placeholder", { 128, 128, 255, 255 });
}

rnd::material_groups::~material_groups()
{
	for (auto& [serial, e] : m_entries) {
		destroy(e);
	}
}

void rnd::material_groups::destroy(entry& e)
{
	m_drv.destroy_bind_group(e.group);
	m_drv.destroy_buffer(e.params);
}

void rnd::material_groups::begin_frame()
{
	++m_frame;
	for (auto it = m_entries.begin(); it != m_entries.end();) {
		if (it->second.last_used + 1 < m_frame) {
			destroy(it->second);
			it = m_entries.erase(it);
		} else {
			++it;
		}
	}
}

rnd::material_groups::entry& rnd::material_groups::acquire(const compiled_material& material)
{
	if (const auto found = m_entries.find(material.serial); found != m_entries.end()) {
		return found->second;
	}

	// The block is the size the shader declares; bytes a compiler did not give stay at the defaults.
	material_params block;
	const auto& bytes = material.runtime.material_params;
	if (!bytes.empty()) {
		std::memcpy(&block, bytes.data(), std::min(bytes.size(), sizeof(material_params)));
	}

	driver::buffer_desc params_desc;
	params_desc.size = sizeof(material_params);
	params_desc.usage = driver::buffer_usage::uniform;
	params_desc.update = driver::buffer_update::static_data;
	params_desc.debug_name = "MaterialParams";
	entry made;
	made.params = m_drv.create_buffer(params_desc);
	m_drv.update_buffer(made.params, 0, std::as_bytes(std::span{ &block, 1 }));

	driver::bind_group_desc desc;
	desc.layout = m_layout;
	desc.entries.push_back({ driver::binding_type::ubo, bindings::material_params_slot, made.params, 0, sizeof(material_params), nullptr });
	for (std::uint32_t slot = 0; slot < bindings::material_texture_count; ++slot) {
		driver::texture_interface* texture = slot < material.runtime.samplers.size() ? material.runtime.samplers[slot] : nullptr;
		desc.entries.push_back({ driver::binding_type::texture, slot, {}, 0, 0, texture ? texture : m_placeholders[slot].get() });
	}
	made.group = m_drv.create_bind_group(desc, driver::lifetime::persistent);
	++m_groups_created;
	return m_entries.emplace(material.serial, made).first->second;
}

void rnd::material_groups::bind(const compiled_material& material)
{
	entry& e = acquire(material);
	e.last_used = m_frame;
	m_drv.set_bind_group(driver::material_set, e.group);
}
