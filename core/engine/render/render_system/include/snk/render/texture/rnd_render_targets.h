#pragma once
#include <snk/common/snk_api.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_handle.h>
#include <snk/driver/rnd_texture_interface.h>
#include <snk/resource/res_tag.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace rnd { class texture_manager; }

namespace rnd::render_targets
{
	struct sampling
	{
		driver::texture_header::FILTERING min = driver::texture_header::FILTERING::NEAREST;
		driver::texture_header::FILTERING mag = driver::texture_header::FILTERING::NEAREST;
		driver::texture_header::WRAPPING wrap = driver::texture_header::WRAPPING::CLAMP_TO_EDGE;
	};

	// The render target stored under `tag`: kept while it has the requested size, otherwise dropped
	// from the cache and created again.
	SNK_API driver::texture_interface* ensure(texture_manager& textures, const res::tag& tag, glm::ivec2 size,
		driver::texture_header::TYPE format, driver::texture_usage usage, const sampling& params = {});

	// A render target named by a stable slot. The slot of a tag stays the same when the target is created again
	// (a resize, a cache clear); `gen` grows each time, so an id kept across that is stale, not a dangling pointer.
	struct target_id
	{
		std::uint32_t slot = 0;	// 0 is null
		std::uint32_t gen = 0;

		constexpr explicit operator bool() const noexcept { return slot != 0; }
		friend constexpr bool operator==(const target_id&, const target_id&) = default;
	};

	// The slots of the render targets of one renderer. The textures stay in the texture manager (they are found
	// by tag there); the registry knows which texture a slot holds right now and when that changed. A texture that
	// the manager dropped without ensure() noticing (remove, clear_cache) is dead for resolve()/current() too: they
	// ask the driver.
	// Main thread only (the thread of the driver), no locking: the passes run there, and a capture from another
	// thread (viewport.capture through MCP) is marshalled to the main thread before it asks (see capture_service).
	// A target is made again when its size, format or usage differs from the request; the sampling is applied on
	// creation only.
	class SNK_API registry
	{
	public:
		// `drv` must outlive the registry.
		explicit registry(driver::driver_interface& drv);

		// The id of the target under `tag`, made or re-made as ensure() does; the generation grows when the
		// texture behind the slot is a different one than before. Null id when the target cannot be made.
		target_id ensure(texture_manager& textures, const res::tag& tag, glm::ivec2 size,
			driver::texture_header::TYPE format, driver::texture_usage usage, const sampling& params = {});

		// The id of the slot of `tag` as it is now; null for a tag that never had a target.
		target_id find(const res::tag& tag) const;

		// The texture of the slot; null for a null or stale id (the target was made again since).
		driver::texture_handle resolve(target_id id) const;
		bool is_current(target_id id) const;

		// The texture of the slot of `tag` right now (find + resolve); null when there is none.
		driver::texture_handle current(const res::tag& tag) const;

	private:
		struct slot
		{
			driver::texture_handle handle;
			std::uint32_t gen = 0;
		};

		driver::driver_interface* m_driver = nullptr;
		std::vector<slot> m_slots;	// slot n is m_slots[n - 1]
		std::unordered_map<res::tag, std::uint32_t> m_by_tag;
	};
}
