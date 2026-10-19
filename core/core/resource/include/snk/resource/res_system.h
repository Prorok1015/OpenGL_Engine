#pragma once
#include <snk/common/common.h>
#include <snk/resource/res_tag.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/adapters/res_adapter_info.hpp>
#include <snk/resource/res_tag_keyed_map.hpp>
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/resource/adapters/res_dependencies.hpp>
#include <snk/resource/resources/res_resource_base.h>
#include <snk/resource/resolvers/res_resolver_interface.hpp>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <mutex>
#include <memory>
#include <set>
#include <shared_mutex>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/snk_api.h>
#include <utility>
#include <vector>

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

	// The value of a res::when_all handle: carries nothing, the handles that were joined hold the resources.
	struct SNK_API when_all_result : resource_entry {};

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
			m_watchers[tag][owner] = watcher{ std::move(cb), ++m_watch_epoch };
		}

		void unwatch(const res::tag& tag, void* owner) {
			std::lock_guard lock(m_cache_mutex);
			if (m_watchers.contains(tag)) {
				m_watchers[tag].erase(owner);
			}
		}

		// Any thread. Drops the tag's cache entries right away; the watchers are called later on the main thread
		// (task_system::drain_main): those registered before the signal and still registered by then.
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

		// Blocks until the resource is ready or failed. Main thread only: a pool worker that waits stalls the pool
		// (asserts on one, except under policy::manual, where the tests pump the lanes themselves). The adapter of a
		// resource that needs others declares them instead (adapter_interface::collect_dependencies).
		template<class RESOURCE>
		auto require_sync(const res::tag& tag) {
			ASSERT_MSG(!(m_tasks.mode() == ds::policy::threaded && ds::task_system::on_worker()),
				"require_sync('{}') on a pool thread: declare it as a dependency of the adapter instead", tag.view());
			return require_resource_internal<RESOURCE, true>(tag, resolve_alias(tag));
		}

		// A handle that is ready once every block is ready, and failed once all of them have finished and any failed:
		// the ones that loaded are still delivered and cached. Prefer res::when_all(...) below.
		res_handle<when_all_result> when_all_blocks(std::vector<std::pair<std::shared_ptr<resource_handle>, res::tag>> blocks);

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
				// Main thread: serializing may wait for field handles, which a pool thread must not do.
				m_tasks.post_main([this, tag, block, alive = m_lifetime.token()]() {
					if (alive.expired()) return;
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
		// A task that never runs (cancelled, shutting down) settles `block` with an error so nobody waits on it forever.
		void run_on_compute(std::function<void()> cb, std::shared_ptr<resource_handle> block);
		void deliver_to_watchers(const res::tag& tag, std::uint64_t signal_epoch);
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

		struct wait_graph;
		struct await_state;

		// The pipeline: resolve -> collect_dependencies (compute) -> require each dependency, no waiting ->
		// deserialize (compute) once all are ready. A block with no dependencies is one compute task.
		void start_loading(const res::tag& tag, const std::shared_ptr<resource_handle>& block, adapter ad);
		void collect_stage(const res::tag& tag, const std::shared_ptr<resource_handle>& block, const data& raw, const adapter& ad);
		void deserialize_stage(const res::tag& tag, const std::shared_ptr<resource_handle>& block, const data& raw, const adapter& ad, const res_deps& deps);
		// Calls `on_done` (any thread) once every block has settled without error, and fails `owner` if any failed.
		// The waits go through the wait graph, which fails the blocks of a cycle instead of letting them wait forever.
		void await_blocks(const std::shared_ptr<resource_handle>& owner, const res::tag& owner_tag,
			std::vector<std::pair<std::shared_ptr<resource_handle>, res::tag>> blocks, std::function<void()> on_done);
		void wait_for_block(const std::shared_ptr<resource_handle>& block) const;

		template<class RESOURCE, bool is_sync>
		auto require_resource_internal(const res::tag& requested, const res::tag& tag) {
			auto found = find_adapter_recursive<RESOURCE>(tag);
			ASSERT_MSG(found, "No adapter found for resource type!");

			auto [block, is_new] = find_or_insert_cache(cache_key{ tag, ds::type_id::make<RESOURCE>() });
			if (is_new) {
				start_loading(tag, block, std::move(found));
			}
			if constexpr (is_sync) {
				wait_for_block(block);
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

		struct watcher {
			reload_callback callback;
			std::uint64_t epoch = 0; // m_watch_epoch value at registration
		};
		std::unordered_map<res::tag, std::unordered_map<void*, watcher>> m_watchers;
		std::uint64_t m_watch_epoch = 0; // bumped by watch() and signal_changed(), guarded by m_cache_mutex

		// Which pending block waits for which, to find cycles (see wait_graph in res_system.cpp).
		std::unique_ptr<wait_graph> m_graph;

		ds::task_system& m_tasks;
		std::mutex m_stop_mutex;
		bool m_stopping = false;
		// Binds the load tasks: the destructor cancels and waits for them before the members they touch go away.
		ds::lifetime m_lifetime;
	};

	// A dependency of `target` as T, for adapter_interface::collect_dependencies.
	template<class T>
	res_dep make_dep(const res::tag& target)
	{
		return res_dep{ target, ds::type_id::make<T>(), [](resource_system& system, const res::tag& tag) {
			return system.require<T>(tag).get_control_block();
		} };
	}

	// Joins resources that are requested separately (for example, what a level needs): ready when all are,
	// failed when all have finished and any failed. Delivers on any thread; use .then(lifetime, cb) for the main one.
	template<class... T>
	res_handle<when_all_result> when_all(resource_system& system, const res_handle<T>&... handles)
	{
		std::vector<std::pair<std::shared_ptr<resource_system::resource_handle>, res::tag>> blocks;
		blocks.reserve(sizeof...(T));
		(blocks.emplace_back(handles.get_control_block(), handles.get_tag()), ...);
		return system.when_all_blocks(std::move(blocks));
	}

	template<class T>
	res_handle<when_all_result> when_all(resource_system& system, const std::vector<res_handle<T>>& handles)
	{
		std::vector<std::pair<std::shared_ptr<resource_system::resource_handle>, res::tag>> blocks;
		blocks.reserve(handles.size());
		for (const auto& handle : handles) {
			blocks.emplace_back(handle.get_control_block(), handle.get_tag());
		}
		return system.when_all_blocks(std::move(blocks));
	}
}