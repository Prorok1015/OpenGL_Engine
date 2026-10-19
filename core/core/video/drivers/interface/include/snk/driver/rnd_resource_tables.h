#pragma once
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
	// Objects of one kind by handle. A slot freed by erase() is handed out again with a higher generation, so the
	// old handle finds nothing. Backends share it for the buffers and bind groups they create.
	template <class Tag, class T>
	class slot_table
	{
	public:
		using handle_type = handle<Tag>;

		handle_type insert(T value)
		{
			std::uint32_t index = 0;
			if (!m_free.empty()) {
				index = m_free.back();
				m_free.pop_back();
			} else {
				m_slots.emplace_back();
				index = static_cast<std::uint32_t>(m_slots.size());
			}
			slot& s = m_slots[index - 1];
			s.value.emplace(std::move(value));
			++m_live;
			return handle_type{ index, s.gen };
		}

		// Null for a handle that names no live object.
		T* find(handle_type h) { return const_cast<T*>(std::as_const(*this).find(h)); }
		const T* find(handle_type h) const
		{
			if (h.index == 0 || h.index > m_slots.size()) {
				return nullptr;
			}
			const slot& s = m_slots[h.index - 1];
			return s.gen == h.gen && s.value ? &*s.value : nullptr;
		}

		bool erase(handle_type h)
		{
			if (!find(h)) {
				return false;
			}
			slot& s = m_slots[h.index - 1];
			s.value.reset();
			++s.gen;
			m_free.push_back(h.index);
			--m_live;
			return true;
		}

		std::size_t live() const { return m_live; }

		template <class Visit>
		void for_each(Visit&& visit)
		{
			for (std::size_t i = 0; i < m_slots.size(); ++i) {
				if (m_slots[i].value) {
					visit(handle_type{ static_cast<std::uint32_t>(i + 1), m_slots[i].gen }, *m_slots[i].value);
				}
			}
		}

	private:
		struct slot
		{
			std::optional<T> value;
			std::uint32_t gen = 1;
		};

		std::vector<slot> m_slots;
		std::vector<std::uint32_t> m_free;
		std::size_t m_live = 0;
	};

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
