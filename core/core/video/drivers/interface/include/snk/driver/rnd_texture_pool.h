#pragma once
#include <snk/common/shader_serial.h>
#include <snk/driver/rnd_resource_tables.h>
#include <snk/driver/rnd_texture_interface.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace rnd::driver
{
	// The generation of a texture handle is split: the high bits name the driver that issued it, the low bits count
	// the reuses of the slot. A handle of another driver (or the handle a validation wrapper's inner driver gave
	// the same texture) therefore fails the generation check of this driver and resolves to nothing.
	//
	// 8 bits of driver id: ids 1..255 are handed out in turn, so the 256th driver of a process shares the id of the
	// first one (two live drivers that far apart in creation order may take each other's handles for their own).
	// 24 bits of generation: a slot reused 2^24 times wraps, and a handle that stale could match again.
	inline constexpr std::uint32_t texture_driver_id_shift = 24;
	inline constexpr std::uint32_t texture_generation_mask = (1u << texture_driver_id_shift) - 1u;

	// The id the next texture_pool will carry in the high bits of its generations (never 0).
	inline std::uint32_t next_texture_driver_tag()
	{
		return next_texture_driver_id() << texture_driver_id_shift;
	}

	// The textures of one driver, by handle. The pool owns them: a texture lives until destroy() (or the pool) and
	// its slot is then freed with a higher generation, so the handle it had resolves to nothing from then on.
	//
	// Thread safety: the slots are guarded by a mutex. The pointer find() returns is only as good as the texture's
	// lifetime, as always: it stays valid until the texture is destroyed.
	class texture_pool
	{
	public:
		texture_pool() : m_slots(texture_generation_mask, next_texture_driver_tag()) {}
		texture_pool(const texture_pool&) = delete;
		texture_pool& operator=(const texture_pool&) = delete;

		// Takes the texture over and stores its handle in it.
		texture_handle adopt(std::unique_ptr<texture_interface> texture)
		{
			texture_interface& adopted = *texture;
			const std::lock_guard lock(m_mutex);
			const texture_handle handle = m_slots.insert(std::move(texture));
			adopted.set_handle(handle);
			return handle;
		}

		// Frees the slot and gives the texture back, so the caller decides where it dies. Null for a handle that
		// names no live texture.
		std::unique_ptr<texture_interface> release(texture_handle handle)
		{
			std::optional<std::unique_ptr<texture_interface>> taken;
			{
				const std::lock_guard lock(m_mutex);
				taken = m_slots.extract(handle);
			}
			if (!taken) {
				return nullptr;
			}
			return std::move(*taken);
		}

		// Null for a handle that names no live texture (null, destroyed, or from another driver).
		texture_interface* find(texture_handle handle) const
		{
			const std::lock_guard lock(m_mutex);
			const std::unique_ptr<texture_interface>* found = m_slots.find(handle);
			return found ? found->get() : nullptr;
		}

		// True for a handle of a texture that has been destroyed: its slot exists but holds no texture of that
		// generation. A handle of another driver is not stale, it is unknown.
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
		mutable std::mutex m_mutex;
		slot_table<texture_tag, std::unique_ptr<texture_interface>> m_slots;
	};
}
