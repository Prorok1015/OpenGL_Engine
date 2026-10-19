#include <snk/render/rnd_per_draw_data.h>
#include <snk/render/skinning/rnd_skinning_manager.h>
#include <snk/common/engine_assert.h>
#include <snk/common/mem_allocator.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <memory_resource>
#include <vector>

namespace {
	constexpr std::size_t initial_objects = 256;
	// 128 was the old per-draw limit; the buffer now holds the matrices of every skeleton of the frame.
	constexpr std::size_t initial_bones = 128;
	constexpr std::size_t empty_skin_bytes = 64;

	std::size_t align_up(std::size_t value, std::size_t alignment)
	{
		return (value + alignment - 1) / alignment * alignment;
	}
}

rnd::per_draw_data::per_draw_data(driver::driver_interface& drv)
	: m_drv(drv)
	, m_layout(drv.create_bind_group_layout(object_layout_desc()))
	, m_stride(align_up(sizeof(object_data), drv.uniform_offset_alignment()))
{
	driver::buffer_desc skin_desc;
	skin_desc.size = empty_skin_bytes;
	skin_desc.usage = driver::buffer_usage::storage;
	skin_desc.update = driver::buffer_update::static_data;
	skin_desc.debug_name = "empty skin indices";
	m_empty_skin = m_drv.create_buffer(skin_desc);
	const std::byte zeros[empty_skin_bytes]{};
	m_drv.update_buffer(m_empty_skin, 0, zeros);

	create_buffers(initial_objects, initial_bones);
}

rnd::per_draw_data::~per_draw_data()
{
	destroy_buffers();
	for (auto& [tag, slot] : m_skins) {
		if (slot.indices) {
			m_drv.destroy_buffer(slot.indices);
		}
	}
	m_drv.destroy_buffer(m_empty_skin);
}

void rnd::per_draw_data::create_buffers(std::size_t objects, std::size_t bones)
{
	m_object_capacity = objects;
	m_bone_capacity = bones;

	driver::buffer_desc ring_desc;
	ring_desc.size = objects * m_stride;
	ring_desc.usage = driver::buffer_usage::uniform;
	ring_desc.update = driver::buffer_update::per_frame;
	ring_desc.debug_name = "object ring";
	m_ring = m_drv.create_buffer(ring_desc);

	driver::buffer_desc bones_desc;
	bones_desc.size = bones * sizeof(glm::mat4);
	bones_desc.usage = driver::buffer_usage::storage;
	bones_desc.update = driver::buffer_update::per_frame;
	bones_desc.debug_name = "bone matrices";
	m_bones = m_drv.create_buffer(bones_desc);

	m_empty_group = make_group(m_empty_skin, empty_skin_bytes);
}

void rnd::per_draw_data::destroy_buffers()
{
	// Groups name the ring and the bones: they go first, and the skins' groups are made again on their next use.
	m_drv.destroy_bind_group(m_empty_group);
	for (auto& [tag, slot] : m_skins) {
		if (slot.group) {
			m_drv.destroy_bind_group(slot.group);
			slot.group = {};
		}
	}
	m_drv.destroy_buffer(m_ring);
	m_drv.destroy_buffer(m_bones);
}

rnd::driver::bind_group_handle rnd::per_draw_data::make_group(driver::buffer_handle skin, std::size_t skin_size)
{
	driver::bind_group_desc desc;
	desc.layout = m_layout;
	desc.entries.push_back({ driver::binding_type::ubo, bindings::object_slot, m_ring, 0, sizeof(object_data), nullptr });
	desc.entries.push_back({ driver::binding_type::ssbo, bindings::bones_slot, m_bones, 0, m_bone_capacity * sizeof(glm::mat4), nullptr });
	desc.entries.push_back({ driver::binding_type::ssbo, bindings::skin_slot, skin, 0, skin_size, nullptr });
	++m_groups_created;
	return m_drv.create_bind_group(desc, driver::lifetime::persistent);
}

void rnd::per_draw_data::upload(std::span<const object_data> objects, std::span<const glm::mat4> bones)
{
	if (objects.size() > m_object_capacity || bones.size() > m_bone_capacity) {
		const std::size_t objects_wanted = std::max(m_object_capacity, std::bit_ceil(objects.size()));
		const std::size_t bones_wanted = std::max(m_bone_capacity, std::bit_ceil(bones.size()));
		destroy_buffers();
		create_buffers(objects_wanted, bones_wanted);
	}

	if (!objects.empty()) {
		// Elements are padded to the stride; the frame allocator takes the staging copy and forgets it at the next frame.
		std::pmr::vector<std::byte> staging(ds::frame_allocator());
		staging.reserve(objects.size() * m_stride);
		staging.resize(objects.size() * m_stride);
		for (std::size_t i = 0; i < objects.size(); ++i) {
			std::memcpy(staging.data() + i * m_stride, &objects[i], sizeof(object_data));
		}
		m_drv.update_buffer(m_ring, 0, staging);
	}
	if (!bones.empty()) {
		m_drv.update_buffer(m_bones, 0, std::as_bytes(bones));
	}
}

void rnd::per_draw_data::release(skin_slot& slot)
{
	if (slot.group) {
		m_drv.destroy_bind_group(slot.group);
		slot.group = {};
	}
	if (slot.indices) {
		m_drv.destroy_buffer(slot.indices);
		slot.indices = {};
	}
	slot.size = 0;
}

rnd::driver::bind_group_handle rnd::per_draw_data::group_for(const res::tag& skin, const skinning_manager* skins)
{
	if (!skin.is_valid() || !skins) {
		return m_empty_group;
	}

	auto found = m_skins.find(skin);
	if (found != m_skins.end() && found->second.checked != skins->generation()) {
		// Some skin was registered since: only the ones whose weights were registered anew are built again.
		found->second.checked = skins->generation();
		if (found->second.version != skins->version(skin)) {
			release(found->second);
			m_skins.erase(found);
			found = m_skins.end();
		}
	}
	if (found == m_skins.end()) {
		skin_slot made;
		made.version = skins->version(skin);
		made.checked = skins->generation();
		if (made.version != 0) {
			const std::vector<std::uint32_t> indices = skins->build_indices(skin);
			driver::buffer_desc desc;
			desc.size = indices.size() * sizeof(std::uint32_t);
			desc.usage = driver::buffer_usage::storage;
			desc.update = driver::buffer_update::static_data;
			desc.debug_name = "skin indices";
			made.indices = m_drv.create_buffer(desc);
			made.size = desc.size;
			m_drv.update_buffer(made.indices, 0, std::as_bytes(std::span{ indices }));
		}
		found = m_skins.emplace(skin, made).first;
	}

	skin_slot& slot = found->second;
	if (!slot.indices) {
		return m_empty_group;
	}
	if (!slot.group) {
		slot.group = make_group(slot.indices, slot.size);
	}
	return slot.group;
}

void rnd::per_draw_data::bind_object(std::uint32_t index, const res::tag& skin, const skinning_manager* skins)
{
	ASSERT_MSG(index < m_object_capacity, "object {} is past the ring of {} elements", index, m_object_capacity);
	const std::uint32_t offset = static_cast<std::uint32_t>(index * m_stride);
	m_drv.set_bind_group(driver::object_set, group_for(skin, skins), std::span{ &offset, 1 });
}
