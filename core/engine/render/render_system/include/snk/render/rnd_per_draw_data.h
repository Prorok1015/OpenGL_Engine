#pragma once
#include <snk/common/snk_api.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/render/rnd_bindings.h>
#include <snk/resource/res_tag.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace rnd
{
	class skinning_manager;

	// What draws read from set 3 (object): ObjectData from a per-frame ring of uniform elements, the frame's
	// bone matrices in one storage buffer, and the index buffer of the skin the draw uses.
	//
	// The ring is a per_frame buffer (the driver keeps one copy per frame in flight) of elements aligned to the
	// uniform offset alignment; a draw picks its element by the dynamic offset of set_bind_group. The bones are
	// written once per frame, a draw finds its skeleton by ObjectData::bone_offset.
	class SNK_API per_draw_data
	{
	public:
		explicit per_draw_data(driver::driver_interface& drv);
		~per_draw_data();
		per_draw_data(const per_draw_data&) = delete;
		per_draw_data& operator=(const per_draw_data&) = delete;

		// Writes the objects and the bone palette of the frame. Call once per frame, before the passes that bind
		// them; both buffers grow when the frame does not fit and keep the size afterwards.
		void upload(std::span<const object_data> objects, std::span<const glm::mat4> bones);

		// Binds set 3 for element `index` of the objects given to upload(). A skinned draw names its skin; a draw
		// without one, or with a skin nobody registered weights for, sees an empty index buffer.
		void bind_object(std::uint32_t index, const res::tag& skin, const skinning_manager* skins);

		// Bytes between two elements of the ring.
		std::size_t object_stride() const { return m_stride; }
		std::size_t object_capacity() const { return m_object_capacity; }
		std::size_t bone_capacity() const { return m_bone_capacity; }
		// Persistent bind groups made so far; grows with distinct skins and with each growth of a buffer.
		std::size_t groups_created() const { return m_groups_created; }

	private:
		struct skin_slot
		{
			driver::buffer_handle indices;
			std::size_t size = 0;
			driver::bind_group_handle group;
		};

		void create_buffers(std::size_t objects, std::size_t bones);
		void destroy_buffers();
		driver::bind_group_handle make_group(driver::buffer_handle skin, std::size_t skin_size);
		driver::bind_group_handle group_for(const res::tag& skin, const skinning_manager* skins);

		driver::driver_interface& m_drv;
		driver::bind_group_layout_handle m_layout;
		std::size_t m_stride = 0;
		std::size_t m_object_capacity = 0;
		std::size_t m_bone_capacity = 0;
		driver::buffer_handle m_ring;
		driver::buffer_handle m_bones;
		driver::buffer_handle m_empty_skin;
		driver::bind_group_handle m_empty_group;
		std::unordered_map<res::tag, skin_slot> m_skins;
		std::size_t m_groups_created = 0;
	};
}
