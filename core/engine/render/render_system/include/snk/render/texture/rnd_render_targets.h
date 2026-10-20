#pragma once
#include <snk/common/snk_api.h>
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
	// by tag there); the registry knows which texture a slot holds right now and when that changed.
	class SNK_API registry
	{
	public:
		// The id of the target under `tag`, made or re-made as ensure() does; the generation grows when the
		// texture behind the slot is a different one than before. Null id when the target cannot be made.
		target_id ensure(texture_manager& textures, const res::tag& tag, glm::ivec2 size,
			driver::texture_header::TYPE format, driver::texture_usage usage, const sampling& params = {});

		// The id of the slot of `tag` as it is now; null for a tag that never had a target.
		target_id find(const res::tag& tag) const;

		// The texture of the slot; null for a null or stale id (the target was made again since).
		driver::texture_handle resolve(target_id id) const noexcept;
		bool is_current(target_id id) const noexcept;

		// The texture of the slot of `tag` right now (find + resolve); null when there is none.
		driver::texture_handle current(const res::tag& tag) const noexcept;

	private:
		struct slot
		{
			driver::texture_handle handle;
			std::uint32_t gen = 0;
		};

		std::vector<slot> m_slots;	// slot n is m_slots[n - 1]
		std::unordered_map<res::tag, std::uint32_t> m_by_tag;
	};
}
