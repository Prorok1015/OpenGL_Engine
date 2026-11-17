#pragma once
#include <snk/common/ds/ds_handle_pool.hpp>
#include <snk/driver/rnd_bind_group.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rnd::driver
{
	// Objects of one kind by handle: a ds::handle_pool whose handles are the driver's typed ones. A slot freed by
	// erase() is handed out again with a higher generation, so the old handle finds nothing. Backends share it for
	// the buffers, bind groups and textures they create.
	template <class Tag, class T>
	using slot_table = ds::handle_pool<T, handle<Tag>>;

	// Bind group layouts by description: the same entries always give the same handle, so a pipeline and a
	// group built apart agree on the layout by comparing handles. Layouts live as long as the cache.
	class bind_group_layout_cache
	{
	public:
		bind_group_layout_handle acquire(const bind_group_layout_desc& desc)
		{
			if (const auto it = m_index.find(desc); it != m_index.end()) {
				return it->second;
			}
			m_layouts.push_back(desc);
			const bind_group_layout_handle handle{ static_cast<std::uint32_t>(m_layouts.size()), generation };
			m_index.emplace(m_layouts.back(), handle);
			return handle;
		}

		// Null for a handle this cache did not hand out.
		const bind_group_layout_desc* find(bind_group_layout_handle handle) const
		{
			if (handle.index == 0 || handle.index > m_layouts.size() || handle.gen != generation) {
				return nullptr;
			}
			return &m_layouts[handle.index - 1];
		}

		std::size_t size() const { return m_layouts.size(); }

	private:
		static constexpr std::uint32_t generation = 1;

		std::deque<bind_group_layout_desc> m_layouts;
		std::unordered_map<bind_group_layout_desc, bind_group_layout_handle, bind_group_layout_desc_hash> m_index;
	};
}
