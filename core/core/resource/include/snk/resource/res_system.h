#pragma once
#include <snk/common/common.h>
#include <snk/resource/res_tag.h>
#include <snk/common/logger/engine_log.h>
#include <snk/resource/adapters/res_adapter_info.hpp>
#include <snk/resource/res_tag_keyed_map.hpp>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/resource/adapters/res_dependencies.hpp>
#include <snk/resource/resources/res_resource_base.h>
#include <snk/resource/resolvers/res_resolver_interface.hpp>
#include <snk/resource/resources/res_resource_handle.hpp>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
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
		// Mounts config_mount_table().
		explicit resource_system(ds::task_system& tasks);
		resource_system(ds::task_system& tasks, mount_table mounts);
		// A layer over `parent` (MODULE_LAYER.md), main thread. It shares the parent's resolvers, mounts and tasks and
		// has its own cache, pins, aliases, adapters, dependency graph and watchers. A request whose type has no adapter
		// in the layer goes to the parent (one shared block); what the layer's own adapters load (the desc types, with a
		// desc adapter on the layer's desc_system) and what is pinned in it lives and dies with the layer. Changes are
		// signalled from the root to every layer. The parent outlives the layer.
		explicit resource_system(resource_system& parent);
		// Cancels the load tasks that have not started and waits for the running ones: main thread only. A layer then
		// reports the resources it loaded that are still referenced (their code may belong to a module being unloaded).
		~resource_system();
		resource_system(const resource_system&) = delete;
		resource_system& operator= (const resource_system&) = delete;
		resource_system(resource_system&&) = delete;
		resource_system& operator= (resource_system&&) = delete;

		// The top (writable) root of res://; empty when res:// is not mounted from directories.
		std::filesystem::path get_resources_path() const;
		// Physical roots behind res://, lowest layer first, top (writable) layer last; the same list the vfs resolver mounts.
		std::vector<std::filesystem::path> get_resource_layers() const;

		// No project open: res:// over resource/base_path (if set) and resource/path, memory:// next to resource/path,
		// engine:// on resource/base_path (if set).
		static mount_table config_mount_table();
		// A project open: res:// over `engine_base` (empty: none) and `content_roots` (highest priority first, as in the
		// manifest; the first one receives writes), cache:// in `cache_dir` with memory:// in its "memory" folder,
		// user:// in `user_dir`, engine:// on `engine_base`. Only builds the table: the caller checks and creates the
		// directories (core::res::resource_init).
		static mount_table project_mount_table(const std::filesystem::path& engine_base,
			const std::vector<std::filesystem::path>& content_roots, const std::filesystem::path& cache_dir,
			const std::filesystem::path& user_dir);
		// resource/base_path as configured, empty when not set; not checked.
		static std::filesystem::path configured_engine_base();

		bool exists(const tag& tag) const {
			const auto& resolvers = root().m_resolvers;
			if (resolvers.find(tag.protocol()) == resolvers.end()) {
				egLOG_WARN("resource/exists", "Protocol '{}' is not supported!", tag.protocol());
				return false;
			}
			return resolvers.at(tag.protocol())->exists(tag);
		}

		// Throws std::logic_error for a tag outside limit_writes_to.
		bool store(const tag& tag, const std::vector<std::byte>& data) {
			check_writable(tag);
			const auto& resolvers = root().m_resolvers;
			if (resolvers.find(tag.protocol()) == resolvers.end()) {
				egLOG_WARN("resource/store", "Protocol '{}' is not supported!", tag.protocol());
				return false;
			}

			return resolvers.at(tag.protocol())->store(tag, data);
		}


		// keep_dependencies retains graph edges until unwatch, without retaining resource data.
		void watch(const res::tag& tag, void* owner, reload_callback cb, bool keep_dependencies = false) {
			std::lock_guard lock(m_cache_mutex);
			m_watchers[tag][owner] = watcher{ std::move(cb), ++m_watch_epoch, keep_dependencies };
		}

		void unwatch(const res::tag& tag, void* owner) {
			std::lock_guard lock(m_cache_mutex);
			if (m_watchers.contains(tag)) {
				m_watchers[tag].erase(owner);
			}
		}

		// Read-only: cached resources that were requested but have neither finished nor failed yet.
		// Used to tell that the scene has settled (deterministic headless runs).
		std::size_t pending_loads() const {
			std::shared_lock lock(m_cache_mutex);
			std::size_t count = 0;
			m_cache.for_each([&count](const cache_entry& entry) {
				if (const auto block = entry.lock()) {
					const auto status = block->status.load();
					if (status != core::res::res_status::ready && status != core::res::res_status::error) {
						++count;
					}
				}
			});
			return count;
		}

		// Any thread. Drops the cache entries of the tag and of everything built from it (the reverse edges of the
		// dependency graph, transitively) right away. The watchers are called later on the main thread
		// (task_system::drain_main), one task per signal: those registered before the signal and still registered by
		// then, each tag of the batch once, parents before the resources that depend on them.
		void signal_changed(const res::tag& tag);

		// Read-only: cached resources of this system (a layer: of the layer only) that someone still references, pinned
		// ones included. For tests and diagnostics.
		std::size_t live_resources() const {
			std::shared_lock lock(m_cache_mutex);
			std::size_t count = 0;
			m_cache.for_each([&count](const cache_entry& entry) {
				if (!entry.expired()) {
					++count;
				}
			});
			return count;
		}

		// Read-only: dependency targets this system keeps for its layers. For tests and diagnostics.
		std::size_t layer_needs_count() const {
			std::lock_guard lock(m_layer_needs_mutex);
			return m_layer_needs.size();
		}

		// Read-only: resources pinned in this system (warmup, pin_resource; a layer: the layer's only). For tests and
		// diagnostics.
		std::size_t pinned_count() const {
			std::shared_lock lock(m_pinning_mutex);
			std::size_t count = 0;
			m_pinned_resources.for_each([&count](const std::shared_ptr<resource_handle>& entry) {
				if (entry) {
					++count;
				}
			});
			return count;
		}

		// The system this one is a layer of; null for the root.
		// Const: a layer's user (a game module) must not write past the layer into the base.
		const resource_system* parent() const { return m_parent; }

		// A layer only: store, erase and pin_resource of a tag not stored yet accept only tags that start with `prefix`
		// (a game module's memory://modules/<name>/, MODULE_LAYER.md); any other tag throws std::logic_error, like a
		// service the module's storage does not expose. The layer's desc_system keeps its inline sub-descs under
		// <prefix>overrides/ (desc::override_root). Empty: no limit. Main thread, before the layer is handed out.
		void limit_writes_to(std::string prefix);
		const std::string& write_prefix() const { return m_write_prefix; }

		// Read-only: how many (tag, dependency) edges the hot reload graph holds. For tests and diagnostics.
		std::size_t dependency_edge_count() const {
			std::shared_lock lock(m_cache_mutex);
			std::size_t count = 0;
			for (const auto& [tag, targets] : m_depends_on) {
				count += targets.size();
			}
			return count;
		}

		// Read-only: how many alias edges register_alias() holds. For tests and diagnostics.
		std::size_t alias_edge_count() const {
			std::shared_lock lock(m_cache_mutex);
			return m_alias_edges.size();
		}

		// Read-only: how many sweeps of the hot reload graph have run. For tests and diagnostics.
		std::size_t edge_sweep_count() const {
			std::shared_lock lock(m_cache_mutex);
			return m_edge_sweeps;
		}

		template<class T, class... ARGS>
		T& registrate_resolver(protocol prt, ARGS... args) {
			ASSERT_MSG(!m_parent, "A resource layer uses the resolvers of its root");
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
			const auto& resolvers = root().m_resolvers;
			if (resolvers.find(tag.protocol()) == resolvers.end()) {
				egLOG_WARN("resource/require", "Protocol '{}' is not supported!", tag.protocol());
				return data{};
			}

			return resolvers.at(tag.protocol())->resolve(tag);
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
			keep_warm(res.get_control_block(), key_for<T>(tag));
		}

		template<class RESOURCE>
		void pin_resource(const res::tag& tag, RESOURCE&& resource) {
			const bool stored = exists(tag);
			if (!stored) {
				check_writable(tag); // a pin that is not stored yet is written out: refused before anything is pinned
			}
			auto block = std::make_shared<resource_handle>();
			block->set_ready(std::make_shared<RESOURCE>(std::forward<RESOURCE>(resource)));
			const cache_key key = key_for<RESOURCE>(tag);
			push_resource_to_strong_cache(key, block);
			push_resource_to_cache(key, block);

			if (!stored) {
				serialize_pinned(tag, block, find_adapter_in_chain<RESOURCE>(tag));
			}
		}

		// Any thread. Removes what store() put under the tag (the resolvers are the root's, so a layer removes it for
		// everyone), for the memory protocol only (false for the others, whose files stay, and for a protocol without a
		// resolver or a tag the resolver does not hold). Then drops the pin and the alias mappings that point at the tag
		// (resolve_alias of their original is the original again; the original's own data stays) in this system and
		// its layers, never in a parent (a layer does not change the base), and signals the tag as changed: its cache
		// entries and those of everything built from it go, watchers are called. An erased original loses its alias
		// mapping the same way (the alias' resolver data stays).
		bool erase(const res::tag& tag);

		void unpin_resource(const res::tag& tag) {
			std::unique_lock lock(m_pinning_mutex);
			m_pinned_resources.erase_tag(tag);
		}

		// Besides copying the cache entries of `original` (the ones the alias lacks), makes `alias` depend on it: a
		// signal_changed(original) evicts the alias too, and reaches what depends on the alias. One alias per
		// original; a new one replaces the edge; a change signalled on the original drops the alias until the next
		// load re-registers it. register_alias(x, x) does nothing.
		void register_alias(const res::tag& original, const res::tag& alias);
		// A layer's own aliases first, then its parent's.
		res::tag resolve_alias(const res::tag& tag) const {
			{
				std::shared_lock lock(m_alias_mutex);
				auto it = m_aliases.find(tag);
				if (it != m_aliases.end()) {
					return it->second;
				}
			}
			return m_parent ? m_parent->resolve_alias(tag) : tag;
		}

	private:
		// Callbacks that run on other systems' blocks or on resolver threads (on_settled) reach this system only through
		// the gate. The destructor closes it before the members go away; a late callback then fails its block instead.
		struct gate {
			std::recursive_mutex mutex;
			resource_system* self = nullptr;

			// False when the gate is closed (fn not called).
			template<class Fn>
			bool with(Fn&& fn) {
				std::lock_guard lock(mutex);
				if (!self) {
					return false;
				}
				fn(*self);
				return true;
			}
		};

		void check_writable(const res::tag& tag) const;
		const resource_system& root() const { return m_parent ? m_parent->root() : *this; }
		resource_system& root() { return m_parent ? m_parent->root() : *this; }
		// signal_changed() on this system and, through m_layers, on its layers.
		void signal_changed_local(const res::tag& tag);
		// erase(): drops the pin and the alias mappings of the tag here and in every layer.
		void forget_erased(const res::tag& tag);

		std::shared_ptr<resource_handle> find_cached(const cache_key& key) {
			std::shared_lock lock(m_cache_mutex);
			const cache_entry* slot = m_cache.find(key);
			return slot ? slot->lock() : nullptr;
		}

		// warmup / pin_resource: not templates, so the callbacks they leave on blocks (the parent's, the main queue) are
		// the engine's code and hold nothing of the caller's binary.
		void keep_warm(const std::shared_ptr<resource_handle>& block, cache_key key);
		void serialize_pinned(const res::tag& tag, std::shared_ptr<resource_handle> block, adapter ad);

		struct await_state;

		// The part of a load of this system that waits for the root's side (a resolver's data, a parent's block, the
		// main queue): what runs next and the block it fills. The root side holds only the id, so nothing it keeps
		// refers to this system's adapters, blocks or resources (which a module's code may have made) once the system
		// is gone; the destructor fails the blocks still parked and drops the rest (MODULE_LAYER.md §5).
		struct parked {
			std::shared_ptr<resource_handle> block;
			std::function<void()> run;
		};
		std::uint64_t park(std::shared_ptr<resource_handle> block, std::function<void()> run);
		std::optional<parked> unpark(std::uint64_t id);
		// Runs the parked step `id` through the gate; nothing when the system is gone or the step was already taken.
		static void run_parked(const std::shared_ptr<gate>& gate, std::uint64_t id);
		// The waits of await_blocks, by id: their dependencies (a parent's blocks too) see only the id.
		std::uint64_t register_await(std::shared_ptr<await_state> state);
		std::shared_ptr<await_state> find_await(std::uint64_t id);
		void drop_await(std::uint64_t id);
		// Destructor: fails what is still parked or awaited and drops it.
		void cancel_parked();

		// A task that never runs (cancelled, shutting down) settles `block` with an error so nobody waits on it forever.
		void run_on_compute(std::function<void()> cb, std::shared_ptr<resource_handle> block);
		void deliver_to_watchers(const res::tag& tag, std::uint64_t signal_epoch);
		// Replaces the dependencies `dependent` is built from. Called before they are requested, so a change that
		// lands in between still reaches `dependent`. A load that was evicted meanwhile (`block` is no longer cached
		// for the tag) records nothing: it must not overwrite the edges of the load that replaced it.
		void record_dependencies(const res::tag& dependent, const std::shared_ptr<resource_handle>& block,
			const std::vector<res_dep>& declared);
		// Drops the outgoing edges of released tags (all cache entries expired, or none left after a signal evicted the
		// tag and nothing watches it), except the ones a tag still in use is built through: a change at the bottom of
		// such a chain must pass through them to reach it. m_cache_mutex held (unique).
		void prune_released_edges();
		// prune_released_edges() plus the bookkeeping that throttles the next sweeps. m_cache_mutex held (unique).
		void sweep_edges();
		// A layer's live resources built on `target` (a dependency the layer recorded): the parents keep the edges of
		// `target` while any layer needs it, though nothing of their own uses it any more, so a change below `target`
		// still reaches the layer through their batch. Counted; the layer holds its m_cache_mutex, the parents' count
		// has its own leaf lock.
		void retain_in_parents(const res::tag& target);
		void release_in_parents(const res::tag& target);
		bool needed_by_layers(const res::tag& tag) const;
		// `root` and everything that depends on it, transitively, each once, parents first. m_cache_mutex held.
		std::vector<res::tag> changed_batch(const res::tag& root) const;
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
			ds::type_id type = ds::type_id::make<RESOURCE>();
			const resource_system* owner = nullptr;
			auto found = find_adapter_in_chain<RESOURCE>(tag, &type, &owner);
			if (m_parent && owner != this) {
				// The most specific adapter is a parent's (or there is none): the parent's block, unless the layer
				// pinned one (warmup and pin_resource also put it in the layer's cache, which a change evicts).
				if (auto pinned = find_cached(cache_key{ tag, type })) {
					if constexpr (is_sync) {
						wait_for_block(pinned);
					}
					return res_handle<RESOURCE>(pinned, requested, &m_tasks);
				}
				return m_parent->require_resource_internal<RESOURCE, is_sync>(requested, tag);
			}
			if (!found) {
				// Not fatal: a format whose importer is not built in (SNK_WITH_ASSIMP=OFF) ends up here. The block below
				// settles with an error, so whoever waits for it fails instead of the process.
				egLOG_ERROR("res/adapter", "No adapter found for '{}' (extension '{}'): the importer for this format is not registered or not built in", tag.view(), tag.extension());
			}

			auto [block, is_new] = find_or_insert_cache(cache_key{ tag, type });
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

		// The most specific adapter for T in this system and its parents: each type level (T, then its base_type ...)
		// is looked up in this system first and then in the parents, so a parent's adapter for the derived type wins over
		// a layer's adapter for its base (world_desc's own adapter over the desc adapter of a layer). `loaded_as` is set
		// to the type the adapter was registered for: a resource whose adapter is its base type's (desc_base's, for every
		// desc) is one cache block shared with that base, not one block per requested type. `owner`: the system the
		// adapter belongs to, which loads and caches the resource.
		template<class T>
		adapter find_adapter_in_chain(const res::tag& tag, ds::type_id* loaded_as = nullptr, const resource_system** owner = nullptr) const {
			for (const resource_system* system = this; system; system = system->m_parent) {
				auto adapter = system->find_adapter<T>(tag.extension());
				if (!adapter) {
					adapter = system->find_adapter<T>("*");
				}
				if (adapter) {
					if (loaded_as) *loaded_as = ds::type_id::make<T>();
					if (owner) *owner = system;
					return adapter;
				}
			}

			if constexpr (requires { typename T::base_type; }) {
				return find_adapter_in_chain<typename T::base_type>(tag, loaded_as, owner);
			}

			return nullptr;
		}

		template<class T>
		cache_key key_for(const res::tag& tag) const {
			ds::type_id type = ds::type_id::make<T>();
			find_adapter_in_chain<T>(tag, &type);
			return cache_key{ tag, type };
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
			bool keep_dependencies = false;
		};
		std::unordered_map<res::tag, std::unordered_map<void*, watcher>> m_watchers;
		std::uint64_t m_watch_epoch = 0; // bumped by watch() and signal_changed(), guarded by m_cache_mutex

		// Edges of the dependency graph between tags, guarded by m_cache_mutex: dependency -> tags built from it
		// (what signal_changed follows) and tag -> its dependencies (to replace them when the tag loads again).
		// Keyed by tag only: a tag has one dependency set whichever adapter type loads it (key_for already maps the
		// desc types of a tag onto one block).
		std::unordered_map<res::tag, std::vector<res::tag>> m_dependents;
		std::unordered_map<res::tag, std::vector<res::tag>> m_depends_on;
		// original -> alias, set by register_alias(): a change of the original evicts the alias as well, and removes the
		// edge together with the m_aliases mapping. Never pruned, and not part of m_depends_on. Guarded by m_cache_mutex;
		// signal_changed() also holds m_alias_mutex (inside it) when it erases.
		std::unordered_map<res::tag, res::tag> m_alias_edges;
		// Released tags are swept out of the graph whenever m_depends_on reaches m_edge_sweep_at (then doubled over what
		// is left) and on a signal: every signal while the graph is below the floor, every k_signal_sweep_every-th
		// above it. The graph stays proportional to what is loaded. All guarded by m_cache_mutex.
		static constexpr std::size_t k_edge_sweep_floor = 256;
		static constexpr std::size_t k_signal_sweep_every = 16;
		std::size_t m_edge_sweep_at = k_edge_sweep_floor;
		std::size_t m_signals_since_sweep = 0;
		std::size_t m_edge_sweeps = 0;
		// Which pending block waits for which, to find cycles (see wait_graph in res_system.cpp).
		std::unique_ptr<wait_graph> m_graph;

		ds::task_system& m_tasks;
		mount_table m_mounts;
		resource_system* m_parent = nullptr;
		std::string m_write_prefix;
		// The layers over this system, for signal_changed() and erase(); a layer adds and removes itself.
		std::mutex m_layers_mutex;
		std::vector<resource_system*> m_layers;
		std::shared_ptr<gate> m_gate = std::make_shared<gate>();
		mutable std::mutex m_layer_needs_mutex;
		std::unordered_map<res::tag, std::size_t> m_layer_needs;
		std::mutex m_parked_mutex;
		std::unordered_map<std::uint64_t, parked> m_parked;
		std::unordered_map<std::uint64_t, std::shared_ptr<await_state>> m_awaits;
		std::uint64_t m_next_parked = 1;
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