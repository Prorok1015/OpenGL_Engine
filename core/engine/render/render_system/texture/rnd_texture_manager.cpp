#include <snk/render/texture/rnd_texture_manager.h>
#include "rnd_texture_2d_desc.h"
#include "rnd_texture_cubemap_desc.h"
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_picture.h>
#include <snk/render/texture/rnd_texture_desc.h>
#include <snk/common/logger/engine_log.h>
#include <snk/common/engine_assert.h>

#include <algorithm>
#include <utility>

rnd::texture_manager::~texture_manager()
{
	ASSERT_MSG(on_owner_thread(), "texture_manager destruction: main thread only");
	for (const auto& user : m_file_users) {
		m_res_sys.unwatch(user.first, this);
	}
}

void rnd::texture_manager::track_files(texture_id id, const texture_desc* description)
{
	ASSERT_MSG(on_owner_thread(), "texture_manager::track_files: main thread only");
	slot& target = m_slots[id.index - 1];
	std::vector<res::tag> files;
	files.reserve(7);
	files.push_back(target.tag);
	auto add_picture = [&](const res::res_handle<res::picture_resource>& picture) {
		const auto& tag = picture.get_tag();
		if (tag.is_valid() && std::ranges::find(files, tag) == files.end()) files.push_back(tag);
	};
	if (const auto* image = dynamic_cast<const texture_2d_desc*>(description)) {
		add_picture(image->get_picture_handle());
	} else if (const auto* cube = dynamic_cast<const texture_cubemap_desc*>(description)) {
		if (cube->get_source_type() == texture_cubemap_desc::SOURCE_TYPE::SINGLE_FILE) {
			add_picture(cube->get_single_image());
		} else {
			const auto& faces = cube->get_faces();
			for (const auto* face : { &faces.right, &faces.left, &faces.bottom, &faces.top, &faces.front, &faces.back })
				add_picture(*face);
		}
	}
	if (files == target.files) return;

	for (const auto& file : target.files) {
		if (std::ranges::find(files, file) != files.end()) continue;
		const auto users = m_file_users.find(file);
		if (users == m_file_users.end()) continue;
		std::erase(users->second, id);
		if (users->second.empty()) {
			m_res_sys.unwatch(file, this);
			m_file_users.erase(users);
		}
	}
	for (const auto& file : files) {
		auto [users, added] = m_file_users.try_emplace(file);
		if (added) {
			m_res_sys.watch(file, this, [this](const res::tag& changed) { on_file_changed(changed); });
		}
		if (std::ranges::find(users->second, id) == users->second.end()) users->second.push_back(id);
	}
	target.files = std::move(files);
}

void rnd::texture_manager::on_file_changed(const res::tag& file)
{
	ASSERT_MSG(on_owner_thread(), "texture_manager::on_file_changed: main thread only");
	const auto found = m_file_users.find(file);
	if (found == m_file_users.end()) return;
	const auto users = found->second;
	for (const auto id : users) {
		const auto& tag = m_slots[id.index - 1].tag;
		if (file != tag) {
			// Picture fields are not graph dependencies. Evict the desc too, otherwise its handles still
			// point at the old picture. Its watch will invalidate the GPU slot on the main-thread queue.
			m_res_sys.signal_changed(tag);
			continue;
		}
		m_descriptions.erase(tag);
		remove(tag);
		egLOG_DEBUG("render/texture", "'{}' changed: texture will be loaded again", tag.view());
	}
}

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
	ASSERT_MSG(on_owner_thread(), "texture_manager::require: main thread only");
	const texture_id id = slot_of(tag);
	slot& target = m_slots[id.index - 1];
	if (target.owner) {
		return id;
	}
	// Generated render targets return above and are never watched as resource-backed textures.
	if (target.files.empty()) track_files(id);

	auto it = m_descriptions.find(tag);
	res::res_handle<rnd::texture_desc> handle;
	if (it == m_descriptions.end()) {
		handle = m_res_sys.require<rnd::texture_desc>(tag);
		m_descriptions[tag] = handle;
	} else {
		handle = it->second;
	}

	if (handle.has_error()) {
		m_descriptions.erase(tag);
		egLOG_ERROR("render/geometry", "Failed to load texture desc with name {0}", tag.view());
	}

	if (!handle.is_ready()) {
		return id;
	}

	track_files(id, handle.get().get());
	auto loader_it = loaders.find(std::string{ handle->get_type() });
	if (loaders.end() != loader_it) {
		if (auto texture = loader_it->second(drv, *handle)) {
			fill(target, std::move(texture));
		}
	}

	return id;
}

rnd::texture_id rnd::texture_manager::generate(const res::tag& tag, rnd::driver::texture_header header)
{
	ASSERT_MSG(on_owner_thread(), "texture_manager::generate: main thread only");
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
	ASSERT_MSG(on_owner_thread(), "texture_manager::binding: main thread only");
	const slot* found = find_slot(id);
	if (!found) {
		return {};
	}
	return { found->owner.handle(), found->generation };
}

const rnd::driver::texture_header& rnd::texture_manager::info(texture_id id) const
{
	static const driver::texture_header none{};
	const slot* found = find_slot(id);
	return found && found->owner ? drv->info(found->owner.handle()) : none;
}

rnd::texture_id rnd::texture_manager::generate(const res::tag& tag, glm::ivec2 size, rnd::driver::texture_header::TYPE channels, std::vector<unsigned char> data)
{
	rnd::driver::texture_header header;
	header.data.extent.width = size.x;
	header.data.extent.height = size.y;
	header.data.format = channels;
	header.data.initial_data = data.data();
	header.data.mip_levels = 1;

	return generate(tag, header);
}

void rnd::texture_manager::remove(const res::tag& tag)
{
	ASSERT_MSG(on_owner_thread(), "texture_manager::remove: main thread only");
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
	ASSERT_MSG(on_owner_thread(), "texture_manager::clear_cache: main thread only");
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
