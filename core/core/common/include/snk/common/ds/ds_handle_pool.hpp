#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ds
{
	// A reference to an object of a handle_pool: the slot (1-based, 0 is null) and the generation the slot had
	// when the object was inserted.
	struct pool_handle
	{
		std::uint32_t index = 0;
		std::uint32_t gen = 0;

		constexpr explicit operator bool() const noexcept { return index != 0; }
		friend constexpr bool operator==(const pool_handle&, const pool_handle&) = default;
	};

	// A slot map. Objects live in slots; erase() frees a slot to a free list and bumps its generation, so the
	// handle the object had finds nothing even after the slot is handed out again.
	//
	// `Handle` is any aggregate with `index` and `gen` members of std::uint32_t (rnd::driver::handle<Tag> is one):
	// the pool builds handles as `Handle{ index, gen }` and never stores anything else of them.
	template <class T, class Handle = pool_handle>
	class handle_pool
	{
	public:
		using handle_type = Handle;

		// The generation is 32 bits and wraps after 2^32 reuses of one slot; a handle that stale could match again,
		// which is accepted. If T's move constructor throws, the pool is left as it was.
		handle_type insert(T value)
		{
			std::uint32_t index = 0;
			if (!m_free.empty()) {
				index = m_free.back();
				m_slots[index - 1].value.emplace(std::move(value));
				m_free.pop_back();
			} else {
				// Room for every slot in the free list first, so erase() never has to allocate.
				m_free.reserve(m_slots.size() + 1);
				m_slots.emplace_back();
				index = static_cast<std::uint32_t>(m_slots.size());
				try {
					m_slots.back().value.emplace(std::move(value));
				} catch (...) {
					m_slots.pop_back();
					throw;
				}
			}
			slot& s = m_slots[index - 1];
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

		// True for a handle whose slot exists but holds no object of that generation: the object was erased,
		// and the slot is free or already used by a newer object. A null handle, or a slot this pool never
		// had, is not stale (it is just unknown).
		bool stale(handle_type h) const
		{
			if (h.index == 0 || h.index > m_slots.size()) {
				return false;
			}
			return find(h) == nullptr;
		}

		// Does not allocate: the free list was reserved by insert().
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
		// Slots ever made, live or free.
		std::size_t capacity() const { return m_slots.size(); }

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
}
