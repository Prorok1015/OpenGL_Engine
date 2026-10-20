#pragma once
#include <snk/driver/rnd_handle.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace rnd
{
	// A texture of the renderer by a name that outlives its GPU object: the slot a texture_manager keeps for a tag.
	// The slot stays when the texture is reloaded, recreated at another size, destroyed, or still loading, so a
	// packet, a material or a pass can hold the id for as long as it likes and ask what is behind it when it draws.
	// Index 0 is "no texture".
	struct texture_id
	{
		std::uint32_t index = 0;

		constexpr bool is_valid() const noexcept { return index != 0; }
		constexpr explicit operator bool() const noexcept { return index != 0; }
		friend constexpr bool operator==(const texture_id&, const texture_id&) = default;
	};

	// What a texture_id names right now: the GPU object (null while there is none: loading, removed) and the
	// generation of the slot, which grows every time the object behind the id changes. Whoever caches something
	// made from the object (a bind group) keys it by (id, generation).
	struct texture_binding
	{
		driver::texture_handle handle;
		std::uint32_t generation = 0;
	};

	// The part of the texture manager that the draw side needs: ids to what is behind them.
	class texture_resolver
	{
	public:
		virtual ~texture_resolver() = default;
		virtual texture_binding binding(texture_id id) const = 0;
	};
}

template <>
struct std::hash<rnd::texture_id>
{
	std::size_t operator()(const rnd::texture_id& id) const noexcept
	{
		return std::hash<std::uint32_t>{}(id.index);
	}
};
