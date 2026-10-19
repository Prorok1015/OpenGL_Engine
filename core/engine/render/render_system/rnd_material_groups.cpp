#include <snk/render/rnd_material_groups.h>
#include <snk/render/shader/rnd_material_params.h>
#include <snk/common/logger/engine_log.h>

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
		if (it->second.last_used + grace_frames < m_frame) {
			destroy(it->second);
			it = m_entries.erase(it);
		} else {
			++it;
		}
	}
}

std::size_t rnd::material_groups::content_hash::operator()(const content_key& key) const noexcept
{
	std::uint64_t h = 14695981039346656037ull;
	const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; h ^= h >> 29; };
	for (const std::byte b : key.block) {
		mix(static_cast<std::uint64_t>(b));
	}
	for (const std::uint64_t texture : key.textures) {
		mix(texture);
	}
	return static_cast<std::size_t>(h);
}

rnd::material_groups::content_key rnd::material_groups::key_of(const compiled_material& material, texture_set& textures) const
{
	// The block is the size the shader declares; bytes a compiler did not give stay at the defaults.
	const material_params defaults;
	content_key key;
	std::memcpy(key.block.data(), &defaults, sizeof(defaults));
	const auto& bytes = material.runtime.material_params;
	if (!bytes.empty()) {
		std::memcpy(key.block.data(), bytes.data(), std::min(bytes.size(), key.block.size()));
	}
	for (std::uint32_t slot = 0; slot < bindings::material_texture_count; ++slot) {
		driver::texture_interface* texture = slot < material.runtime.samplers.size() ? material.runtime.samplers[slot] : nullptr;
		textures[slot] = texture ? texture : m_placeholders[slot].get();
		key.textures[slot] = textures[slot]->serial();
	}
	return key;
}

rnd::material_groups::entry& rnd::material_groups::acquire(const compiled_material& material)
{
	texture_set textures{};
	const content_key key = key_of(material, textures);
	if (const auto found = m_entries.find(key); found != m_entries.end()) {
		return found->second;
	}

	if (material.runtime.samplers.size() > bindings::material_texture_count) {
		egLOG_WARN("render/material", "a material has {} samplers, set 2 has {} texture slots: the extra ones are not bound",
			material.runtime.samplers.size(), bindings::material_texture_count);
	}
	material_params block;
	std::memcpy(&block, key.block.data(), sizeof(block));

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
		desc.entries.push_back({ driver::binding_type::texture, slot, {}, 0, 0, textures[slot] });
	}
	made.group = m_drv.create_bind_group(desc, driver::lifetime::persistent);
	++m_groups_created;
	return m_entries.emplace(key, made).first->second;
}

void rnd::material_groups::bind(const compiled_material& material)
{
	entry& e = acquire(material);
	e.last_used = m_frame;
	m_drv.set_bind_group(driver::material_set, e.group);
}
