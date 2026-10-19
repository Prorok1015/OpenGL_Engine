#pragma once
#include <snk/driver/rnd_resource_tables.h>
#include <snk/driver/rnd_texture_interface.h>

#include <cstddef>
#include <memory>
#include <mutex>

namespace rnd::driver
{
	// The textures of one driver by handle. The driver owns a pool for the lifetime of its textures: a texture
	// keeps the pool alive through the callback adopt() gives it, so a texture that outlives its driver still
	// leaves the pool cleanly.
	//
	// Thread safety: the slots are guarded by a mutex, because a texture may be destroyed on whichever thread
	// drops its last owner. The pointer find() returns is only as good as the texture's lifetime, as always.
	//
	// The pool does not own the textures. It only knows where each one is while it lives; destroying a texture
	// frees its slot and bumps the generation, so the handle it had resolves to nothing from then on.
	class texture_pool : public std::enable_shared_from_this<texture_pool>
	{
	public:
		// Pools are shared (adopt() hands each texture a reference), so they are made here and nowhere else.
		static std::shared_ptr<texture_pool> create() { return std::shared_ptr<texture_pool>(new texture_pool()); }

		// Registers a freshly made texture and stores its handle in it.
		texture_handle adopt(texture_interface& texture)
		{
			texture_handle handle;
			{
				const std::lock_guard lock(m_mutex);
				handle = m_slots.insert(&texture);
			}
			texture.assign_handle(handle, [pool = shared_from_this()](texture_handle gone) {
				const std::lock_guard lock(pool->m_mutex);
				pool->m_slots.erase(gone);
			});
			return handle;
		}

		// Null for a handle that names no live texture (null, destroyed, or from another driver).
		texture_interface* find(texture_handle handle) const
		{
			const std::lock_guard lock(m_mutex);
			texture_interface* const* found = m_slots.find(handle);
			return found ? *found : nullptr;
		}

		// True for a handle of a texture that has been destroyed: its slot exists but holds no texture of that generation.
		bool stale(texture_handle handle) const
		{
			const std::lock_guard lock(m_mutex);
			return m_slots.stale(handle);
		}

		// The description of a texture; an empty one for a handle that names no live texture.
		const texture_header& info(texture_handle handle) const
		{
			static const texture_header none{};
			const texture_interface* texture = find(handle);
			return texture ? texture->info() : none;
		}

		std::size_t live() const
		{
			const std::lock_guard lock(m_mutex);
			return m_slots.live();
		}

	private:
		texture_pool() = default;

		mutable std::mutex m_mutex;
		slot_table<texture_tag, texture_interface*> m_slots;
	};
}
