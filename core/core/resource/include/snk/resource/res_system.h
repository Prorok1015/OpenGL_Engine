#pragma once
#include <snk/common/common.h>
#include <snk/resource/res_tag.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/adapters/res_adapter_info.hpp>
#include <snk/resource/res_tag_keyed_map.hpp>
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/resource/resources/res_resource_base.h>
#include <snk/resource/resolvers/res_resolver_interface.hpp>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/snk_api.h>

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

	class SNK_API resource_system
	{
	public:
		using resource_handle = core::res::res_resource_block;
		using cache_entry = std::weak_ptr<resource_handle>;

		using protocol = std::string_view;
		using extension = adapter_info;

		using data = core::res::res_resolver_interface::async_raw_data;
		using resolver = std::shared_ptr<core::res::res_resolver_interface>;

		using adapter = std::shared_ptr<core::res::adapter_interface>;

		using reload_callback = std::function<void(const res::tag&)>;

		using adapter_key = std::pair<ds::type_id, std::string>;
	public:
		// Adapter work runs on `tasks` (lane::compute); watcher callbacks and res_handle::then are delivered through
		// its main-thread queue. `tasks` must outlive this system, and so must the thread that drains it.
		explicit resource_system(ds::task_system& tasks);
		// Cancels the load tasks that have not started and waits for the running ones: main thread only.
		~resource_system();
		resource_system(const resource_system&) = delete;
		resource_system& operator= (const resource_system&) = delete;
		resource_system(resource_system&&) = delete;
		resource_system& operator= (resource_system&&) = delete;

		static std::filesystem::path get_resources_path();
		// Physical roots behind res://, lowest layer first, top (writable) layer last; the same list the vfs resolver mounts.
		static std::vector<std::filesystem::path> get_resource_layers();

		bool exists(const tag& tag) const {
			if (m_resolvers.find(tag.protocol()) == m_resolvers.end()) {
				egLOG_WARN("resource/exists", "Protocol '{}' is not supported!", tag.protocol());
				return false;
			}
			return m_resolvers.at(tag.protocol())->exists(tag);
		}

		bool store(const tag& tag, const std::vector<std::byte>& data) {
			if (m_resolvers.find(tag.protocol()) == m_resolvers.end()) {
				egLOG_WARN("resource/store", "Protocol '{}' is not supported!", tag.protocol());
				return false;
			}

			return m_resolvers.at(tag.protocol())->store(tag, data);
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

		// Any thread. Drops the tag's cache entries right away; the watchers are called later on the main thread
		// (task_system::drain_main), and only those still registered by then.
		void signal_changed(const res::tag& tag);

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
				m_adapters[std::pair{ extension.resource_type, std::string{ "*" }}] = adapter_new;

				if (extension.magic_number != 0) {
					m_extensions_with_magic.insert(std::pair{ extension.resource_type, std::string{ "*" }});
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
				auto it = m_adapters.find(std::pair{ extension.resource_type, std::string{ "*" } });
				if (it != m_adapters.end()) {
					m_adapter_infos.erase(it->second);
					m_adapters.erase(it);
				}

				if (extension.magic_number != 0)
					m_extensions_with_magic.erase(std::pair{ extension.resource_type, std::string{ "*" } });
			}
		}

		data fetch_data(const tag& tag) const
		{
			if (m_resolvers.find(tag.protocol()) == m_resolvers.end()) {
				egLOG_WARN("resource/require", "Protocol '{}' is not supported!", tag.protocol());
				return data{};
			}

			return m_resolvers.at(tag.protocol())->resolve(tag);
		}

		template<class RESOURCE>
		auto require(const res::tag& tag) {
			return require_resource_internal<RESOURCE, false>(tag, resolve_alias(tag));
		}

		template<class RESOURCE>
		auto require_sync(const res::tag& tag) {
			return require_resource_internal<RESOURCE, true>(tag, resolve_alias(tag));
		}

		template<class T>
		void warmup(const res::tag& tag)
		{
			res::res_handle<T> res = require<T>(tag);
			res.get_control_block()->on_settled([this, res, tag](auto&) {
				if (res.has_error()) return;
				push_resource_to_strong_cache(cache_key{ tag, ds::type_id::make<T>() }, res.get_control_block());
			});
		}

		template<class RESOURCE>
		void pin_resource(const res::tag& tag, RESOURCE&& resource) {
			auto block = std::make_shared<resource_handle>();
			block->set_ready(std::make_shared<RESOURCE>(std::forward<RESOURCE>(resource)));
			const cache_key key{ tag, ds::type_id::make<RESOURCE>() };
			push_resource_to_strong_cache(key, block);
			push_resource_to_cache(key, block);

			if (!exists(tag)) {
				run_on_compute([this, tag, block]() {
					try {
						auto adapter = find_adapter_recursive<RESOURCE>(tag);
						if (!adapter) {
							throw std::runtime_error("No adapter found for resource type");
						}
						std::vector<std::byte> raw_data = adapter->serialize(tag, block->data);
						store(tag, raw_data);
					} catch (const std::exception& e) {
						egLOG_ERROR("res/error", "Failed to auto-serialize {0}: {1}", tag.string(), e.what());
					}
				});
			}
		}

		void unpin_resource(const res::tag& tag) {
			std::unique_lock lock(m_pinning_mutex);
			m_pinned_resources.erase_tag(tag);
		}

		void register_alias(const res::tag& original, const res::tag& alias);
		res::tag resolve_alias(const res::tag& tag) const {
			std::shared_lock lock(m_alias_mutex);
			auto it = m_aliases.find(tag);
			if (it != m_aliases.end()) {
				return it->second;
			}
			return tag;
		}

	private:
		void run_on_compute(std::function<void()> cb);
		void deliver_to_watchers(const res::tag& tag);
		// Atomic find-or-insert: concurrent requests for one tag share a single block, only the creator starts the load.
		std::pair<std::shared_ptr<resource_handle>, bool> find_or_insert_cache(const cache_key& key) {
			std::unique_lock lock(m_cache_mutex);
			auto& slot = m_cache[key];
			if (auto sp = slot.lock()) {
				return { std::move(sp), false };
			}
			auto created = std::make_shared<resource_handle>();
			slot = created;
			return { std::move(created), true };
		}

		void push_resource_to_cache(const cache_key& key, std::shared_ptr<resource_handle> resource) {
			std::unique_lock lock(m_cache_mutex);
			m_cache[key] = resource;
		}

		void push_resource_to_strong_cache(const cache_key& key, std::shared_ptr<resource_handle> resource)
		{
			std::unique_lock lock(m_pinning_mutex);
			m_pinned_resources[key] = resource;
		}

		template<typename T, bool is_sync>
		void start_loading_t(res::tag tag, std::shared_ptr<resource_handle> final_cb) {
			// The cache slot already holds final_cb: any escape from here would leave waiters blocked forever.
			try {
				start_loading_unguarded<T, is_sync>(tag, final_cb);
			} catch (const std::exception& e) {
				egLOG_ERROR("res/resolver", "Failed to start loading {0}: {1}", tag.view(), e.what());
				final_cb->set_error(std::string{ "Load error: " } + e.what());
			} catch (...) {
				egLOG_ERROR("res/resolver", "Failed to start loading {}: unknown exception", tag.view());
				final_cb->set_error("Load error: unknown exception");
			}
		}

		template<typename T, bool is_sync>
		void start_loading_unguarded(const res::tag& tag, const std::shared_ptr<resource_handle>& final_cb) {
			auto resolver_it = m_resolvers.find(tag.protocol());
			if (resolver_it == m_resolvers.end()) {
				egLOG_ERROR("res/resolver", "Protocol '{0}' is not supported, cannot load {1}", tag.protocol(), tag.view());
				final_cb->set_error(std::string{ "Unsupported protocol: " } + std::string{ tag.protocol() });
				return;
			}

			auto raw_data_block = resolver_it->second->resolve(tag);
			if (!raw_data_block) {
				egLOG_ERROR("res/resolver", "Resolver returned no data block for {}", tag.view());
				final_cb->set_error("Resolver returned no data");
				return;
			}

			auto adapter_cb = [this, tag, final_cb, raw_data_block]() {
				if (raw_data_block->has_error()) {
					final_cb->set_error(std::string{ "Resolver error: " } + raw_data_block->error_msg);
					egLOG_ERROR("res/resolver", "Failed to resolve resource {0}: {1}", tag.view(), raw_data_block->error_msg);
					return;
				}

				try {
					auto adapter = find_adapter_recursive<T>(tag);
					if (!adapter) {
						throw std::runtime_error("No adapter found for resource type");
					}

					final_cb->status = core::res::res_status::processing;
					auto parsed_obj = adapter->deserialize(tag, raw_data_block->data);

					final_cb->set_ready(std::move(parsed_obj));
				} catch (const std::exception& e) {
					final_cb->set_error(std::string{ "Adapter error: " } + e.what());
					egLOG_ERROR("res/adapter", "Failed to deserialize resource {0}: {1}", tag.view(), e.what());
				} catch (...) {
					final_cb->set_error("Adapter error: unknown exception");
					egLOG_ERROR("res/adapter", "Failed to deserialize resource {}: unknown exception", tag.view());
				}
			};

			if constexpr (is_sync)
			{
				raw_data_block->wait();
				adapter_cb();
			} else {
				raw_data_block->on_settled([this, adapter_cb](auto&) {
					run_on_compute(adapter_cb);
				});
			}
		}

		template<class RESOURCE, bool is_sync>
		auto require_resource_internal(const res::tag& requested, const res::tag& tag) {
			ASSERT_MSG(find_adapter_recursive<RESOURCE>(tag), "No adapter found for resource type!");

			auto [block, is_new] = find_or_insert_cache(cache_key{ tag, ds::type_id::make<RESOURCE>() });
			if (is_new) {
				start_loading_t<RESOURCE, is_sync>(tag, block);
			}
			// The cache is keyed by the alias target, but the handle remembers what the caller asked for,
			// so serializing it writes the tag the caller knows (an alias target may be RAM-only).
			return res_handle<RESOURCE>(block, requested, &m_tasks);
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
		mutable std::shared_mutex m_alias_mutex;

		std::set<adapter_key> m_extensions_with_magic;
		std::unordered_map<adapter_key, adapter> m_adapters;
		std::unordered_map<adapter, extension> m_adapter_infos;

		std::unordered_map<res::tag, res::tag> m_aliases;

		std::unordered_map<protocol, resolver> m_resolvers;
		tag_keyed_map<cache_entry> m_cache;
		tag_keyed_map<std::shared_ptr<resource_handle>> m_pinned_resources;

		std::unordered_map<res::tag, std::unordered_map<void*, reload_callback>> m_watchers;

		ds::task_system& m_tasks;
		std::mutex m_stop_mutex;
		bool m_stopping = false;
		// Binds the load tasks: the destructor cancels and waits for them before the members they touch go away.
		ds::lifetime m_lifetime;
	};
}