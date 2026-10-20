#include <snk/render/texture/rnd_render_targets.h>
#include <snk/render/texture/rnd_texture_manager.h>

rnd::driver::texture_interface* rnd::render_targets::ensure(texture_manager& textures, const res::tag& tag, glm::ivec2 size,
	driver::texture_header::TYPE format, driver::texture_usage usage, const sampling& params)
{
	if (auto* existing = textures.find(tag)) {
		if (existing->width() == static_cast<uint32_t>(size.x) && existing->height() == static_cast<uint32_t>(size.y)
			&& existing->format() == format && existing->get_usage() == usage) {
			return existing;
		}
		textures.remove(tag);
	}

	driver::texture_header header;
	header.data.extent.width = size.x;
	header.data.extent.height = size.y;
	header.data.format = format;
	header.usage = usage;
	header.wrap = params.wrap;
	header.min = params.min;
	header.mag = params.mag;
	return textures.generate_texture(tag, header);
}

rnd::render_targets::registry::registry(driver::driver_interface& drv)
	: m_driver(&drv)
{
}

rnd::render_targets::target_id rnd::render_targets::registry::ensure(texture_manager& textures, const res::tag& tag, glm::ivec2 size,
	driver::texture_header::TYPE format, driver::texture_usage usage, const sampling& params)
{
	const driver::texture_interface* texture = render_targets::ensure(textures, tag, size, format, usage, params);
	if (!texture) {
		return {};
	}
	std::uint32_t& index = m_by_tag[tag];
	if (index == 0) {
		m_slots.emplace_back();
		index = static_cast<std::uint32_t>(m_slots.size());
	}
	slot& entry = m_slots[index - 1];
	const driver::texture_handle handle = texture->handle();
	if (entry.handle != handle) {
		entry.handle = handle;
		++entry.gen;
	}
	return { index, entry.gen };
}

rnd::render_targets::target_id rnd::render_targets::registry::find(const res::tag& tag) const
{
	const auto it = m_by_tag.find(tag);
	if (it == m_by_tag.end()) {
		return {};
	}
	return { it->second, m_slots[it->second - 1].gen };
}

rnd::driver::texture_handle rnd::render_targets::registry::resolve(target_id id) const
{
	if (!is_current(id)) {
		return {};
	}
	return m_slots[id.slot - 1].handle;
}

bool rnd::render_targets::registry::is_current(target_id id) const
{
	if (id.slot == 0 || id.slot > m_slots.size()) {
		return false;
	}
	const slot& entry = m_slots[id.slot - 1];
	// The texture can be gone without ensure() having seen it (texture_manager::remove, clear_cache).
	return entry.gen == id.gen && m_driver->is_alive(entry.handle);
}

rnd::driver::texture_handle rnd::render_targets::registry::current(const res::tag& tag) const
{
	return resolve(find(tag));
}
