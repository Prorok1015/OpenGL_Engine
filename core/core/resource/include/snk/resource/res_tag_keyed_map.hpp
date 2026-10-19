#pragma once
#include <snk/resource/res_tag.h>
#include <snk/common/ds/ds_type_id.hpp>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace res {
	struct cache_key
	{
		res::tag tag;
		ds::type_id type;

		bool operator==(const cache_key&) const = default;
	};
}

namespace std {
	template<> struct hash<res::cache_key> {
		std::size_t operator()(const res::cache_key& k) const {
			const std::size_t h1 = std::hash<res::tag>{}(k.tag);
			const std::size_t h2 = std::hash<ds::type_id>{}(k.type);
			return h1 ^ (h2 + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (h1 << 6) + (h1 >> 2));
		}
	};
}

namespace res
{
	// (tag, type) -> V with a secondary tag -> types index: per-tag operations (change signal, unpin, alias copy)
	// touch only that tag's entries instead of scanning the whole cache. Not thread-safe: the owner locks.
	template<class V>
	class tag_keyed_map
	{
	public:
		// The existing slot or a new default-constructed one.
		V& operator[](const cache_key& key)
		{
			auto [it, inserted] = m_entries.try_emplace(key);
			if (inserted) {
				m_types[key.tag].push_back(key.type);
			}
			return it->second;
		}

		[[nodiscard]] V* find(const cache_key& key)
		{
			auto it = m_entries.find(key);
			return it == m_entries.end() ? nullptr : &it->second;
		}

		void erase_tag(const tag& tag)
		{
			auto index = m_types.find(tag);
			if (index == m_types.end()) {
				return;
			}
			for (const ds::type_id type : index->second) {
				m_entries.erase(cache_key{ tag, type });
			}
			m_types.erase(index);
		}

		// Copies every entry of `from` under `to`, leaving alone the ones `to` already holds and is_live() accepts.
		template<class IsLive>
		void copy_tag(const tag& from, const tag& to, IsLive&& is_live)
		{
			auto index = m_types.find(from);
			if (index == m_types.end()) {
				return;
			}
			const std::vector<ds::type_id> types = index->second; // the inserts below may rehash m_types
			for (const ds::type_id type : types) {
				auto source = m_entries.find(cache_key{ from, type });
				if (source == m_entries.end()) {
					continue;
				}
				if (const V* existing = find(cache_key{ to, type }); existing && is_live(*existing)) {
					continue;
				}
				V value = source->second;
				(*this)[cache_key{ to, type }] = std::move(value);
			}
		}

	private:
		std::unordered_map<cache_key, V> m_entries;
		std::unordered_map<tag, std::vector<ds::type_id>> m_types;
	};
}
