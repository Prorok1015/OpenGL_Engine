#pragma once
#include <snk/driver/rnd_render_pass.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace rnd::driver
{
	// The attachments a framebuffer was built from: backend identity plus mip and layer of every one.
	// Load and store ops are not part of it, the same framebuffer serves any ops.
	struct framebuffer_key
	{
		struct slot
		{
			std::uint64_t object = 0;	// backend identity of the texture, 0 = nothing attached
			std::uint32_t mip = 0;
			std::uint32_t layer = 0;

			friend bool operator==(const slot&, const slot&) = default;
		};

		static constexpr std::size_t depth_slot = render_pass_desc::max_colors;

		std::array<slot, render_pass_desc::max_colors + 1> slots{};	// colors first, depth/stencil last
		std::uint8_t color_count = 0;	// a null color still occupies its draw buffer

		friend bool operator==(const framebuffer_key&, const framebuffer_key&) = default;
	};

	struct framebuffer_key_hash
	{
		std::size_t operator()(const framebuffer_key& key) const noexcept
		{
			std::uint64_t h = 0xcbf29ce484222325ull ^ key.color_count;
			const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; };
			for (const auto& s : key.slots) {
				mix(s.object);
				mix((static_cast<std::uint64_t>(s.mip) << 32) | s.layer);
			}
			return static_cast<std::size_t>(h);
		}
	};

	// `object_of(texture_handle)` gives the backend identity of a texture (0 for null).
	template <class ObjectOf>
	framebuffer_key make_framebuffer_key(const render_pass_desc& desc, ObjectOf&& object_of)
	{
		framebuffer_key key;
		key.color_count = static_cast<std::uint8_t>(desc.colors.size());
		for (std::size_t i = 0; i < desc.colors.size(); ++i) {
			const attachment_desc& a = desc.colors[i];
			key.slots[i] = { object_of(a.texture), a.mip, a.layer };
		}
		if (desc.depth_stencil) {
			const attachment_desc& a = *desc.depth_stencil;
			key.slots[framebuffer_key::depth_slot] = { object_of(a.texture), a.mip, a.layer };
		}
		return key;
	}

	// Framebuffers by attachment set. A framebuffer is built on its first use and found again afterwards.
	// The owner evicts every entry that mentions a texture when that texture is destroyed: backends reuse
	// object names, so a stale entry would silently match a different texture.
	template <class Framebuffer>
	class framebuffer_cache
	{
	public:
		template <class Create>
		Framebuffer& acquire(const framebuffer_key& key, Create&& create)
		{
			if (const auto it = m_entries.find(key); it != m_entries.end()) {
				++m_hits;
				return it->second;
			}
			++m_misses;
			return m_entries.emplace(key, create()).first->second;
		}

		// Drops the entries built from `object`; `destroy(Framebuffer&)` runs for each. Returns how many.
		template <class Destroy>
		std::size_t evict(std::uint64_t object, Destroy&& destroy)
		{
			if (object == 0) {
				return 0;
			}
			std::size_t evicted = 0;
			for (auto it = m_entries.begin(); it != m_entries.end();) {
				bool uses = false;
				for (const auto& s : it->first.slots) {
					uses = uses || s.object == object;
				}
				if (uses) {
					destroy(it->second);
					it = m_entries.erase(it);
					++evicted;
				} else {
					++it;
				}
			}
			return evicted;
		}

		template <class Destroy>
		void clear(Destroy&& destroy)
		{
			for (auto& entry : m_entries) {
				destroy(entry.second);
			}
			m_entries.clear();
		}

		std::size_t size() const { return m_entries.size(); }
		// Passes that found their framebuffer.
		std::size_t hits() const { return m_hits; }
		// Framebuffers built so far.
		std::size_t misses() const { return m_misses; }

	private:
		std::unordered_map<framebuffer_key, Framebuffer, framebuffer_key_hash> m_entries;
		std::size_t m_hits = 0;
		std::size_t m_misses = 0;
	};
}
