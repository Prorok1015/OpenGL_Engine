#pragma once
#include <snk/driver/rnd_resource_tables.h>
#include <snk/driver/rnd_texture_interface.h>

#include <cstddef>
#include <memory>

namespace rnd::driver
{
	// The textures of one driver by handle. The driver owns a pool for the lifetime of its textures: a texture
	// keeps the pool alive through the callback adopt() gives it, so a texture that outlives its driver still
	// leaves the pool cleanly.
	//
	// The pool does not own the textures. It only knows where each one is while it lives; destroying a texture
	// frees its slot and bumps the generation, so the handle it had resolves to nothing from then on.
	class texture_pool : public std::enable_shared_from_this<texture_pool>
	{
	public:
		static std::shared_ptr<texture_pool> make() { return std::make_shared<texture_pool>(); }

		// Registers a freshly made texture and stores its handle in it.
		texture_handle adopt(texture_interface& texture)
		{
			const texture_handle handle = m_slots.insert(&texture);
			texture.assign_handle(handle, [pool = shared_from_this()](texture_handle gone) { pool->m_slots.erase(gone); });
			return handle;
		}

		// Null for a handle that names no live texture (null, destroyed, or from another driver).
		texture_interface* find(texture_handle handle) const
		{
			texture_interface* const* found = m_slots.find(handle);
			return found ? *found : nullptr;
		}

		// True for a handle of a texture that has been destroyed: its slot exists but holds no texture of that generation.
		bool stale(texture_handle handle) const { return m_slots.stale(handle); }

		// The description of a texture; an empty one for a handle that names no live texture.
		const texture_header& info(texture_handle handle) const
		{
			static const texture_header none{};
			const texture_interface* texture = find(handle);
			return texture ? texture->info() : none;
		}

		std::size_t live() const { return m_slots.live(); }

	private:
		slot_table<texture_tag, texture_interface*> m_slots;
	};
}
