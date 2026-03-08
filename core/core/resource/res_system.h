#pragma once
#include "common.h"
#include "res_tag.h"
#include "logger/engine_log.h"
#include "adapters/res_adapter_info.hpp"
#include "adapters/res_adapter_worker_base.hpp"
#include "adapters/res_adapter_interface.hpp"
#include "resources/res_resource_base.h"
#include "resolvers/res_resolver_interface.hpp"
#include "resources/res_resource_handle.hpp"
#include <iostream>
#include <set>
#include <shared_mutex>

namespace std {
	template<> struct hash<std::pair<ds::type_id, std::string>> {
		std::size_t operator()(const std::pair<ds::type_id, std::string>& k) const {
			return std::hash<ds::type_id>{}(k.first) ^ std::hash<std::string>{}(k.second);
		}
	};
}

namespace res
{
	template<class T>
	using res_handle = core::res::res_handle<T>;

	class resource_system
	{
	public:
		static std::string get_absolut_path(const tag& tag);
		template<class T>
		using resource_handle_t = core::res::res_control_block<std::shared_ptr<T>>;
		using resource_handle = core::res::res_control_block_base;
		using cache_entry = std::weak_ptr<resource_handle>;

		using protocol = std::string_view;
		using extension = adapter_info;

		using data = core::res::res_resolver_interface::async_raw_data;
		using resolver = std::shared_ptr<core::res::res_resolver_interface>;

		using adapter = std::shared_ptr<core::res::adapter_interface>;

		using reload_callback = std::function<void(const res::tag&)>;

		using adapter_key = std::pair<ds::type_id, std::string>;
	public:
		resource_system();
		~resource_system() = default;
		resource_system(const resource_system&) = delete;
		resource_system& operator= (const resource_system&) = delete;
		resource_system(resource_system&&) = delete;
		resource_system& operator= (resource_system&&) = delete;

		static std::filesystem::path get_resources_path();

		bool exists(const tag& tag) const {
			if (m_resolvers.find(tag.protocol()) == m_resolvers.end()) {
				egLOG("resource/exists", "Protocol '{}' is not supported!", tag.protocol());
				return false;
			}
			return m_resolvers.at(tag.protocol())->exists(tag);
		}

		bool store(const tag& tag, const std::vector<std::byte>& data) {
			if (m_resolvers.find(tag.protocol()) == m_resolvers.end()) {
				egLOG("resource/store", "Protocol '{}' is not supported!", tag.protocol());
				return false;
			}

			return m_resolvers.at(tag.protocol())->store(tag, data);
		}

		void set_adapter_worker(std::unique_ptr<core::res::adapter_worker_base>&& worker) {
			m_adapter_worker = std::move(worker);
			m_adapter_worker->start();
		}

		void watch(const res::tag& tag, void* owner, reload_callback cb) {
			std::lock_guard lock(m_cache_mutex);
			m_watchers[tag][owner] = std::move(cb);
		}

		void unwatch(const res::tag& tag, void* owner) {
			std::lock_guard lock(m_cache_mutex);
			if (m_watchers.contains(tag)) {
				m_watchers[tag].erase(owner);
			}
		}

		void signal_changed(const res::tag& tag) {
			m_cache.erase(tag);

			if (m_watchers.contains(tag)) {
				auto callbacks = m_watchers.at(tag);

				for (auto& [owner, callback] : callbacks) {
					callback(tag);
				}
			}
		}

		template<class T, class... ARGS>
		T& registrate_resolver(protocol prt, ARGS... args) {
			ASSERT_MSG(m_resolvers.find(prt) == m_resolvers.end(), "That protocol already has resolver!");
			return *std::static_pointer_cast<T>(m_resolvers[prt] = std::make_unique<T>(prt, std::forward<ARGS>(args)...));
		}

		void unregistrate_resolver(protocol prt) {
			m_resolvers.erase(prt);
		}

		template<class T, class... ARGS>
		T& registrate_adapter(extension extension, ARGS&&... args) {
			auto adapter_new = std::make_shared<T>(std::forward<ARGS>(args)...);
			m_adapter_infos[adapter_new] = extension;

			if (extension.extensions.empty()) {
				m_adapters[std::pair{ extension.resource_type, "*"s}] = adapter_new;

				if (extension.magic_number != 0) {
					m_extensions_with_magic.insert(std::pair{ extension.resource_type, "*"s});
				}
			}

			for (auto& ext : extension.extensions) {
				m_adapters[std::pair{ extension.resource_type, ext }] = adapter_new;

				if (extension.magic_number != 0) {
					m_extensions_with_magic.insert(std::pair{ extension.resource_type, ext });
				}
			}
			return *adapter_new;
		}

		void unregistrate_adapter(extension extension) {
			for(auto& ext : extension.extensions) {
				auto it = m_adapters.find(std::pair{ extension.resource_type, ext });
				if (it != m_adapters.end())
				{
					m_adapter_infos.erase(it->second);
					m_adapters.erase(it);
				}
				if (extension.magic_number != 0)
					m_extensions_with_magic.erase(std::pair{ extension.resource_type, ext });
			}

			if (extension.extensions.empty()) {
				auto it = m_adapters.find(std::pair{ extension.resource_type, "*"s });
				if (it != m_adapters.end()) {
					m_adapter_infos.erase(it->second);
					m_adapters.erase(it);
				}

				if (extension.magic_number != 0)
					m_extensions_with_magic.erase(std::pair{ extension.resource_type, "*"s });
			}
		}

		data fetch_data(const tag& tag) const
		{
			if (m_resolvers.find(tag.protocol()) == m_resolvers.end()) {
				egLOG("resource/require", "Protocol '{}' is not supported!", tag.protocol());
				return data{};
			}

			return m_resolvers.at(tag.protocol())->resolve(tag);
		}

		template<class RESOURCE>
		auto require(const res::tag& tag) {
			return require_resource_internal<RESOURCE, false>(resolve_alias(tag));
		}

		template<class RESOURCE>
		auto require_sync(const res::tag& tag) {
			return require_resource_internal<RESOURCE, true>(resolve_alias(tag));
		}

		template<class T>
		void warmup(const res::tag& tag)
		{
			res::res_handle<T> res = require<T>(tag);
			res.then([this, res, tag](auto _) {
				if (res.has_error()) return;
				push_resource_to_strong_cache(tag, res.get_control_block());
			});
		}

		template<class RESOURCE>
		void pin_resource(const res::tag& tag, RESOURCE&& resource) {

			auto block = std::make_shared<resource_handle_t<RESOURCE>>();
			block->set_ready(std::make_shared<RESOURCE>(std::forward<RESOURCE>(resource)));
			push_resource_to_strong_cache(tag, block);
			push_resource_to_cache(tag, block);

			if (!exists(tag)) {
				post_adapter_work([this, tag, block]() {
					try {
						auto adapter = find_adapter_recursive<RESOURCE>(tag);
						ASSERT_MSG(adapter, "No adapter found for resource type!");
						std::vector<std::byte> raw_data = adapter->serialize(tag, block->data);
						store(tag, raw_data);
					} catch (const std::exception& e) {
						egLOG("res/error", "Failed to auto-serialize {0}: {1}", tag.string(), e.what());
					}
				});
			}
		}

		void unpin_resource(const res::tag& tag) {
			std::unique_lock lock(m_pinning_mutex);
			m_pinned_resources.erase(tag);
		}

		void register_alias(const res::tag& original, const res::tag& alias);
		const res::tag& resolve_alias(const res::tag& tag) const {
			auto it = m_aliases.find(tag);
			if (it != m_aliases.end()) {
				return it->second;
			}
			return tag;
		}

	private:
		void post_adapter_work(std::function<void()> cb);
		std::shared_ptr<resource_handle> try_get_cached_resource(const tag& tag) {
			std::shared_lock lock(m_cache_mutex);
			auto it = m_cache.find(tag);
			if (it != m_cache.end()) {
				if (auto sp = it->second.lock()) {
					return sp;
				}
				m_cache.erase(it);
			}
			return nullptr;
		}

		void push_resource_to_cache(const tag& tag, std::shared_ptr<resource_handle> resource) {
			std::unique_lock lock(m_cache_mutex);
			m_cache[tag] = resource;
		}

		void push_resource_to_strong_cache(const res::tag& tag, std::shared_ptr<resource_handle> resource)
		{
			std::unique_lock lock(m_pinning_mutex);
			m_pinned_resources[tag] = resource;
		}

		template<typename T, bool is_sync>
		void start_loading_t(res::tag tag, std::shared_ptr<resource_handle_t<T>> final_cb) {
			auto& resolver = m_resolvers[tag.protocol()];

			auto raw_data_block = resolver->resolve(tag);
			ASSERT_MSG(raw_data_block, "resource data not found");

			auto adapter_cb = [this, tag, final_cb, raw_data_block]() {
				if (raw_data_block->status == core::res::res_status::error) {
					final_cb->set_error("Resolver error: "s + raw_data_block->error_msg);
					return;
				}

				try {
					auto adapter = find_adapter_recursive<T>(tag);
					ASSERT_MSG(adapter, "No adapter found for resource type!");

					final_cb->status = core::res::res_status::processing;
					auto parsed_obj = ds::polymorphic_cast<T>(adapter->deserialize(tag, raw_data_block->data));

					final_cb->set_ready(std::move(parsed_obj));
				}
				catch (const std::exception& e) {
					final_cb->set_error("Adapter error: "s + e.what());
				}
			};

			if constexpr (is_sync)
			{
				raw_data_block->wait();
				adapter_cb();
			} else {
				raw_data_block->then([this, adapter_cb](auto& raw_block) {
					post_adapter_work(adapter_cb);
				});
			}
		}

		template<class RESOURCE, bool is_sync>
		auto require_resource_internal(const res::tag& tag) {
			ASSERT_MSG(find_adapter_recursive<RESOURCE>(tag), "No adapter found for resource type!");

			if (auto cached_res = try_get_cached_resource(tag)) {
				return res_handle<RESOURCE>(
					ds::polymorphic_pointer_cast<resource_handle_t<RESOURCE>>(cached_res),
					tag
				);
			}
			auto final_cb = std::make_shared<resource_handle_t<RESOURCE>>();
			push_resource_to_cache(tag, final_cb);
			start_loading_t<RESOURCE, is_sync>(tag, final_cb);
			return res_handle<RESOURCE>(final_cb, tag);
		}

		template<class T>
		adapter find_adapter_recursive(const res::tag& tag) const {			
			if (auto adapter = find_adapter<T>(tag.extension())) {
				return adapter;
			}

			if (auto adapter = find_adapter<T>("*")) {
				return adapter;
			}

			if constexpr (requires { typename T::base_type; }) {
				return find_adapter_recursive<typename T::base_type>(tag);
			}

			return nullptr;
		}

		template<class T>
		adapter find_adapter(const std::string_view ext) const {
			auto key = adapter_key{ ds::type_id::make<T>(), std::string(ext) };
			if (m_extensions_with_magic.contains(key)) {
				// Need to check magic number
			}
			auto it = m_adapters.find(key);
			if (it != m_adapters.end()) return it->second;
			return nullptr;
		}


	private:
		mutable std::shared_mutex m_cache_mutex;
		mutable std::shared_mutex m_pinning_mutex;

		std::set<adapter_key> m_extensions_with_magic;
		std::unordered_map<adapter_key, adapter> m_adapters;
		std::unordered_map<adapter, extension> m_adapter_infos;

		std::unordered_map<res::tag, res::tag> m_aliases;

		std::unordered_map<protocol, resolver> m_resolvers;
		std::unordered_map<res::tag, cache_entry> m_cache;
		std::unordered_map<res::tag, std::shared_ptr<resource_handle>> m_pinned_resources;

		std::unordered_map<res::tag, std::unordered_map<void*, reload_callback>> m_watchers;
		std::unique_ptr<core::res::adapter_worker_base> m_adapter_worker = nullptr;
	};

	resource_system& get_system();
}