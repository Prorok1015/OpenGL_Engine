#pragma once
#include <snk/driver/rnd_pipeline.h>

#include <cstddef>
#include <deque>
#include <unordered_map>
#include <utility>

namespace rnd::driver
{
	// Pipelines by description: one description gives one handle, however often it is asked for. A backend
	// keeps what it derives from the description (`Pipeline`) next to the immutable copy of the description.
	// Pipelines are never destroyed, so a handle stays valid as long as the cache lives.
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
			m_entries.push_back(entry{ desc, create(desc) });
			const pipeline_handle handle{ static_cast<std::uint32_t>(m_entries.size()), generation };
			m_index.emplace(m_entries.back().desc, handle);
			return handle;
		}

		// Null for a handle this cache did not hand out.
		const entry* find(pipeline_handle handle) const
		{
			if (handle.index == 0 || handle.index > m_entries.size() || handle.gen != generation) {
				return nullptr;
			}
			return &m_entries[handle.index - 1];
		}

		std::size_t size() const { return m_entries.size(); }
		// Requests that found their pipeline.
		std::size_t hits() const { return m_hits; }
		// Pipelines built so far.
		std::size_t misses() const { return m_misses; }

	private:
		static constexpr std::uint32_t generation = 1;

		// A deque keeps the entries in place, so a `const pipeline_desc&` handed out stays valid while the cache grows.
		std::deque<entry> m_entries;
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
