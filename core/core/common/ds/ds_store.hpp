#pragma once
#include "ds_type_id.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ds {

	// Services are destroyed in reverse registration order, so a service may hold references
	// to services registered before it.
	class app_data_storage
	{
	public:
		app_data_storage() = default;
		explicit app_data_storage(app_data_storage* parent)
			: m_parent(parent) {
		}
		app_data_storage(app_data_storage&& other) = default;
		app_data_storage& operator=(app_data_storage&& other) {
			if (this != &other) {
				destroy_all();
				m_entries = std::move(other.m_entries);
				m_index = std::move(other.m_index);
				m_parent = other.m_parent;
			}
			return *this;
		}
		app_data_storage(const app_data_storage&) = delete;
		app_data_storage& operator=(const app_data_storage&) = delete;
		~app_data_storage() { destroy_all(); }

		template<class T>
		bool has_value() const {
			return find_entry(type_key<T>(), type_name<T>()) != nullptr;
		}

		template<class T>
		std::shared_ptr<T> require_parent_shared() const {
			if (m_parent) {
				return m_parent->template require_shared<T>();
			}
			return nullptr;
		}

		template<class T>
		std::shared_ptr<T> require_shared() const {
			if (const entry* found = find_entry(type_key<T>(), type_name<T>())) {
				return std::static_pointer_cast<T>(found->object);
			}
			return nullptr;
		}

		template<class T>
		T& require() const {
			const entry* found = find_entry(type_key<T>(), type_name<T>());
			if (!found) {
				// Hard error in every build type: a missing service must not turn into a null dereference.
				throw std::runtime_error(std::string{ "ds::app_data_storage::require: service not found: " } + std::string{ type_name<T>() });
			}
			return *static_cast<T*>(found->object.get());
		}

		template<class T, class... A>
		T& construct(A&&... args) {
			return add<T, T>(std::forward<A>(args)...);
		}

		template<class KEY, class INSTANCE, class... A>
		KEY& construct(A&&... args) {
			return add<KEY, INSTANCE>(std::forward<A>(args)...);
		}

		template<class T>
		void destruct() {
			const std::uint64_t key = type_key<T>();
			auto it = m_index.find(key);
			if (it == m_index.end() || m_entries[it->second].name != type_name<T>()) {
				throw std::logic_error(std::string{ "ds::app_data_storage::destruct: service not registered: " } + std::string{ type_name<T>() });
			}
			// Take the entry out first, so the object is destroyed after the index is consistent again.
			entry removed = std::move(m_entries[it->second]);
			m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(it->second));
			rebuild_index();
		}

		std::size_t size() const noexcept {
			return m_entries.size();
		}

	private:
		struct entry {
			std::uint64_t key;
			std::string_view name;
			std::shared_ptr<void> object;
		};

		template<class KEY, class INSTANCE, class... A>
		KEY& add(A&&... args) {
			ensure_absent(type_key<KEY>(), type_name<KEY>());
			std::shared_ptr<KEY> object = std::make_shared<INSTANCE>(std::forward<A>(args)...);
			KEY& ref = *object;
			insert(type_key<KEY>(), type_name<KEY>(), std::move(object));
			return ref;
		}

		void ensure_absent(std::uint64_t key, std::string_view name) const {
			auto it = m_index.find(key);
			if (it == m_index.end()) {
				return;
			}
			const entry& existing = m_entries[it->second];
			if (existing.name == name) {
				throw std::logic_error(std::string{ "ds::app_data_storage::construct: service already registered: " } + std::string{ name });
			}
			throw std::logic_error(std::string{ "ds::app_data_storage: key collision between " } + std::string{ existing.name } + " and " + std::string{ name });
		}

		void insert(std::uint64_t key, std::string_view name, std::shared_ptr<void> object) {
			ensure_absent(key, name);
			m_index.emplace(key, m_entries.size());
			m_entries.push_back(entry{ key, name, std::move(object) });
		}

		void rebuild_index() {
			m_index.clear();
			for (std::size_t i = 0; i < m_entries.size(); ++i) {
				m_index.emplace(m_entries[i].key, i);
			}
		}

		// Looks in this storage first, then in the parents.
		const entry* find_entry(std::uint64_t key, std::string_view name) const {
			for (const app_data_storage* storage = this; storage != nullptr; storage = storage->m_parent) {
				auto it = storage->m_index.find(key);
				if (it != storage->m_index.end() && storage->m_entries[it->second].name == name) {
					return &storage->m_entries[it->second];
				}
			}
			return nullptr;
		}

		void destroy_all() {
			while (!m_entries.empty()) {
				m_index.erase(m_entries.back().key);
				m_entries.pop_back();
			}
		}

		std::vector<entry> m_entries;
		std::unordered_map<std::uint64_t, std::size_t> m_index;
		app_data_storage* m_parent = nullptr;
	};
}