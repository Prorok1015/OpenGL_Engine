#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_picture.h>
#include <snk/render/texture/rnd_texture_desc.h>
#include <snk/common/logger/engine_log.h>

#include <utility>

rnd::texture_id rnd::texture_manager::slot_of(const res::tag& tag)
{
	if (const auto it = m_ids.find(tag); it != m_ids.end()) {
		return it->second;
	}
	slot made;
	made.tag = tag;
	m_slots.push_back(std::move(made));
	const texture_id id{ static_cast<std::uint32_t>(m_slots.size()) };
	m_ids.emplace(tag, id);
	return id;
}

const rnd::texture_manager::slot* rnd::texture_manager::find_slot(texture_id id) const noexcept
{
	if (!id.is_valid() || id.index > m_slots.size()) {
		return nullptr;
	}
	return &m_slots[id.index - 1];
}

void rnd::texture_manager::fill(slot& s, driver::texture_owner texture)
{
	s.owner = std::move(texture);
	++s.generation;
}

void rnd::texture_manager::empty(slot& s)
{
	s.owner.reset();
	++s.generation;
}

rnd::texture_id rnd::texture_manager::require(const res::tag& tag)
{
	const texture_id id = slot_of(tag);
	slot& target = m_slots[id.index - 1];
	if (target.owner) {
		return id;
	}

	auto it = loading.find(tag);
	res::res_handle<rnd::texture_desc> handle;
	if (it == loading.end()) {
		handle = m_res_sys.require<rnd::texture_desc>(tag);
		loading[tag] = handle;
	} else {
		handle = it->second;
	}

	if (handle.has_error()) {
		loading.erase(tag);
		egLOG_ERROR("render/geometry", "Failed to load texture desc with name {0}", tag.view());
	}

	if (!handle.is_ready()) {
		return id;
	}

	auto loader_it = loaders.find(std::string{ handle->get_type() });
	if (loaders.end() != loader_it) {
		if (auto texture = loader_it->second(drv, *handle)) {
			loading.erase(tag);
			fill(target, std::move(texture));
		}
	}

	return id;
}

rnd::texture_id rnd::texture_manager::generate(const res::tag& tag, rnd::driver::texture_header header)
{
	const texture_id id = slot_of(tag);
	slot& target = m_slots[id.index - 1];
	if (!target.owner) {
		fill(target, rnd::driver::make_texture(*drv, header));
	}
	return id;
}

rnd::texture_id rnd::texture_manager::id_of(const res::tag& tag) const
{
	const auto it = m_ids.find(tag);
	return it != m_ids.end() ? it->second : texture_id{};
}

bool rnd::texture_manager::is_ready(texture_id id) const
{
	const slot* found = find_slot(id);
	return found && found->owner;
}

rnd::texture_binding rnd::texture_manager::binding(texture_id id) const
{
	const slot* found = find_slot(id);
	if (!found) {
		return {};
	}
	return { found->owner.handle(), found->generation };
}

rnd::driver::texture_interface* rnd::texture_manager::generate_texture(const res::tag& tag, rnd::driver::texture_header header)
{
	return find_slot(generate(tag, header))->owner.get();
}

rnd::driver::texture_interface* rnd::texture_manager::generate_texture(const res::tag& tag, glm::ivec2 size, rnd::driver::texture_header::TYPE channels, std::vector<unsigned char> data)
{
	rnd::driver::texture_header header;
	header.data.extent.width = size.x;
	header.data.extent.height = size.y;
	header.data.format = channels;
	header.data.initial_data = data.data();
	header.data.mip_levels = 1;

	return generate_texture(tag, header);
}

rnd::driver::texture_interface* rnd::texture_manager::find(const res::tag& tag) const
{
	const slot* found = find_slot(id_of(tag));
	return found ? found->owner.get() : nullptr;
}

void rnd::texture_manager::remove(const res::tag& tag)
{
	const auto it = m_ids.find(tag);
	if (it == m_ids.end()) {
		return;
	}
	slot& target = m_slots[it->second.index - 1];
	if (target.owner) {
		empty(target);
		++m_generation; // a render-target resize removes its texture too, which costs one material rebuild (perf only)
	}
}

void rnd::texture_manager::clear_cache()
{
	bool removed = false;
	for (auto& target : m_slots) {
		if (target.owner && target.tag.protocol() != res::tag::memory) {
			empty(target);
			removed = true;
		}
	}
	if (removed) {
		++m_generation;
	}
}
