#pragma once
#include <snk/driver/rnd_pipeline.h>

#include <cstddef>
#include <deque>
#include <vector>
#include <unordered_map>
#include <utility>

namespace rnd::driver
{
	// Pipelines by description: one description gives one handle, however often it is asked for. A backend
	// keeps what it derives from the description (`Pipeline`) next to the immutable copy of the description.
	// A pipeline lives until erase_if drops it (its shader is going away); its slot is reused by a later one under
	// a newer generation, so the handle of a dropped pipeline names nothing.
	template <class Pipeline>
	class pipeline_cache
	{
	public:
		struct entry
		{
			pipeline_desc desc;
			Pipeline backend;
		};

		// `create(const pipeline_desc&)` builds the backend part, once per distinct description.
		template <class Create>
		pipeline_handle acquire(const pipeline_desc& desc, Create&& create)
		{
			if (const auto it = m_index.find(desc); it != m_index.end()) {
				++m_hits;
				return it->second;
			}
			++m_misses;
			entry built{ desc, create(desc) };
			std::uint32_t index = 0;
			if (m_free.empty()) {
				m_slots.push_back(slot{ std::move(built), 1, true });
				index = static_cast<std::uint32_t>(m_slots.size());
			} else {
				index = m_free.back();
				m_free.pop_back();
				slot& reused = m_slots[index - 1];
				reused.value = std::move(built);
				reused.live = true;
			}
			const pipeline_handle handle{ index, m_slots[index - 1].gen };
			m_index.emplace(m_slots[index - 1].value.desc, handle);
			return handle;
		}

		// Drops the pipelines whose description `drop` accepts; returns how many.
		template <class Drop>
		std::size_t erase_if(Drop&& drop)
		{
			std::size_t dropped = 0;
			for (std::size_t i = 0; i < m_slots.size(); ++i) {
				slot& s = m_slots[i];
				if (!s.live || !drop(s.value.desc)) {
					continue;
				}
				m_index.erase(s.value.desc);
				s.value = entry{};
				s.live = false;
				if (++s.gen == 0) {
					s.gen = 1;
				}
				m_free.push_back(static_cast<std::uint32_t>(i + 1));
				++dropped;
			}
			return dropped;
		}

		// Null for a handle this cache did not hand out.
		const entry* find(pipeline_handle handle) const
		{
			if (handle.index == 0 || handle.index > m_slots.size()) {
				return nullptr;
			}
			const slot& found = m_slots[handle.index - 1];
			return found.live && found.gen == handle.gen ? &found.value : nullptr;
		}

		// The pipelines that are alive.
		std::size_t size() const { return m_index.size(); }
		// Requests that found their pipeline.
		std::size_t hits() const { return m_hits; }
		// Pipelines built so far.
		std::size_t misses() const { return m_misses; }

	private:
		struct slot
		{
			entry value;
			std::uint32_t gen = 1;
			bool live = false;
		};

		// A deque keeps the entries in place, so a `const pipeline_desc&` handed out stays valid while the cache grows.
		std::deque<slot> m_slots;
		std::vector<std::uint32_t> m_free;	// indices (1-based) of dropped slots
		std::unordered_map<pipeline_desc, pipeline_handle, pipeline_desc_hash> m_index;
		std::size_t m_hits = 0;
		std::size_t m_misses = 0;
	};

	// What pipeline_info answers for a handle that names no pipeline.
	inline const pipeline_desc& empty_pipeline_desc()
	{
		static const pipeline_desc empty;
		return empty;
	}
}
