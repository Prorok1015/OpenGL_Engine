#pragma once
#include <snk/application/app_module_entry.h>
#include <snk/application/app_module_interface.h>
#include <snk/application/app_module_layer.h>
#include <snk/application/app_module_source.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace app::modules {

	// added -> registered -> initialized -> shut_down; unloaded is reached only through
	// module_loader::unload(). register_services may be skipped (added -> initialized directly).
	enum class module_state
	{
		added,
		registered,
		initialized,
		shut_down,
		unloaded
	};

	class module_loader {
	private:
		// Closes the source before deleting it.
		struct source_closer {
			void operator()(module_source* source) const {
				source->close();
				delete source;
			}
		};
		using source_ptr = std::unique_ptr<module_source, source_closer>;

		struct entry {
			// Declared first, so it is closed after the module and its layer are destroyed.
			source_ptr source{};
			module_ptr module;
			module_state state = module_state::added;
			std::size_t size_before = 0; // storage.size() right before this module registered
			std::size_t provided = 0;    // services it left in the storage after register + initialize
			// A layered() module's layer; declared after `module`, so it goes first when the entry is destroyed.
			std::unique_ptr<module_layer> layer{};
		};

		std::vector<entry> m_entries;
		std::vector<std::string> m_unloaded; // names; the module objects are gone
		bool m_sorted = false;

		[[noreturn]] static void fail(std::string message) {
			egLOG_ERROR("app", "{}", message);
			throw std::logic_error(std::move(message));
		}

		entry* find(std::string_view name) {
			const auto it = std::ranges::find_if(m_entries, [&](const entry& e) { return e.module->name() == name; });
			return it == m_entries.end() ? nullptr : &*it;
		}

		static std::string_view state_name(module_state s) {
			switch (s) {
			case module_state::added: return "added";
			case module_state::registered: return "registered";
			case module_state::initialized: return "initialized";
			case module_state::shut_down: return "shut_down";
			case module_state::unloaded: return "unloaded";
			}
			return "?";
		}

		// The storage the module registers into: its layer's, or the root.
		static ds::app_data_storage& storage_of(entry& e, ds::app_data_storage& data) {
			return e.layer ? e.layer->storage() : data;
		}

		void create_layer(entry& e, ds::app_data_storage& data) {
			if (!e.module->layered() || e.layer) {
				return;
			}
			const auto factory = data.require_shared<module_layer_factory>();
			if (!factory) {
				fail(std::format("module_loader: cannot load '{}': it needs a layer and the storage has no module_layer_factory", e.module->name()));
			}
			e.layer = factory->create(data, e.module->name());
		}

		void shutdown_entry(entry& e, ds::app_data_storage& data) {
			e.state = module_state::shut_down;
			try {
				e.module->shutdown_services(storage_of(e, data));
			} catch (const std::exception& ex) {
				egLOG_ERROR("app", "module '{}' threw during shutdown: {}", e.module->name(), ex.what());
			} catch (...) {
				egLOG_ERROR("app", "module '{}' threw unknown exception during shutdown", e.module->name());
			}
			// Right away, not with the entry: the layer belongs to services of modules that shut down after this one.
			e.layer.reset();
		}

		// Shuts down the given entries (indices into m_entries) in reverse order.
		void shutdown_entries(const std::vector<std::size_t>& indices, ds::app_data_storage& data) {
			for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
				shutdown_entry(m_entries[*it], data);
			}
		}

		// Stable topological sort on depends_on() (Kahn): dependencies first, and among modules
		// that are ready at the same time the earliest-inserted one goes first. Register,
		// initialize and shutdown all use this one order (EPIC-49 US-49-7). Re-sorting an
		// already valid order is the identity, so loaded modules never move.
		void ensure_sorted() {
			if (m_sorted) {
				return;
			}
			const std::size_t n = m_entries.size();
			std::unordered_map<std::string_view, std::size_t> index_of;
			for (std::size_t i = 0; i < n; ++i) {
				const auto [it, inserted] = index_of.emplace(m_entries[i].module->name(), i);
				ASSERT_MSG(inserted, "module_loader: duplicate module name");
			}

			std::vector<int> in_degree(n, 0);
			std::vector<std::vector<std::size_t>> dependents(n);
			for (std::size_t i = 0; i < n; ++i) {
				for (const auto dep : m_entries[i].module->depends_on()) {
					const auto it = index_of.find(dep);
					ASSERT_MSG(it != index_of.end(), "module_loader: dependency on unknown module");
					dependents[it->second].push_back(i);
					++in_degree[i];
				}
			}

			std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
			for (std::size_t i = 0; i < n; ++i) {
				if (in_degree[i] == 0) {
					ready.push(i);
				}
			}

			std::vector<std::size_t> order;
			order.reserve(n);
			while (!ready.empty()) {
				const std::size_t cur = ready.top();
				ready.pop();
				order.push_back(cur);
				for (const std::size_t d : dependents[cur]) {
					if (--in_degree[d] == 0) {
						ready.push(d);
					}
				}
			}
			ASSERT_MSG(order.size() == n, "module_loader: dependency cycle detected");

			std::vector<entry> sorted;
			sorted.reserve(n);
			for (const std::size_t idx : order) {
				sorted.push_back(std::move(m_entries[idx]));
			}
			m_entries = std::move(sorted);
			m_sorted = true;
		}

	public:
		void add_module(std::unique_ptr<module_interface> module) {
			m_entries.push_back(make_entry(nullptr, module_ptr{ module.release() }));
			m_sorted = false;
		}

		// Current state of a module, nullopt if the loader never saw that name.
		std::optional<module_state> state(std::string_view name) const {
			const auto it = std::ranges::find_if(m_entries, [&](const entry& e) { return e.module->name() == name; });
			if (it != m_entries.end()) {
				return it->state;
			}
			if (std::ranges::find(m_unloaded, name) != m_unloaded.end()) {
				return module_state::unloaded;
			}
			return std::nullopt;
		}

		void register_all_services(ds::app_data_storage& data) {
			ensure_sorted();
			for (auto& e : m_entries) {
				if (e.state != module_state::added) {
					continue;
				}
				e.size_before = data.size();
				create_layer(e, data);
				e.module->register_services(storage_of(e, data));
				e.state = module_state::registered;
			}
		}

		// If a module throws, the modules initialized by this call are shut down in reverse
		// order and the exception is rethrown. The throwing module itself is not shut down: it
		// never finished initializing and is expected to clean up after itself.
		void initialize_all_services(ds::app_data_storage& data) {
			ensure_sorted();
			std::vector<std::size_t> done;
			done.reserve(m_entries.size());
			try {
				for (std::size_t i = 0; i < m_entries.size(); ++i) {
					auto& e = m_entries[i];
					if (e.state != module_state::added && e.state != module_state::registered) {
						continue;
					}
					if (e.state == module_state::added) {
						e.size_before = data.size();
						create_layer(e, data);
					}
					e.module->initialize_services(storage_of(e, data));
					e.state = module_state::initialized;
					e.provided = data.size() - e.size_before;
					done.push_back(i);
				}
			} catch (...) {
				shutdown_entries(done, data);
				throw;
			}
		}

		// Loads one module after startup: every module it depends on must already be initialized,
		// otherwise nothing is registered and std::logic_error is thrown. The module is appended
		// (its dependencies are all ahead of it), registered and initialized; if either phase
		// throws the exception propagates and nothing is kept. If initialize_services throws, the
		// module has already registered services, so it is shut down (a throw there is logged),
		// the storage size is checked against the pre-load size (error log if it differs) and the
		// module is dropped, which lets the same module be loaded again later.
		void load(std::unique_ptr<module_interface> module, ds::app_data_storage& data) {
			load(module_ptr{ module.release() }, data);
		}

		// Same, for a module created by a module binary's entry point. Refused (std::logic_error,
		// nothing created) when the entry is incomplete or its api major version, compiler or build
		// flags do not match this host. The module is later destroyed through the entry's own
		// destroy function, so the binary that allocated it frees it.
		void load(const module_entry& exports, ds::app_data_storage& data) {
			load_entry(make_entry(nullptr, create_module(exports, "module")), data);
		}

		void load(module_ptr module, ds::app_data_storage& data) {
			load_entry(make_entry(nullptr, std::move(module)), data);
		}

		// Same, for a module whose code comes from `source` (a shared library, or compiled in). The source is
		// opened here and closed after the module is destroyed: when it is unloaded, when the loader goes away,
		// or right away when the load fails (std::logic_error when the source cannot be opened or its entry is refused).
		void load(std::unique_ptr<module_source> source, ds::app_data_storage& data) {
			ASSERT_MSG(source, "module_loader::load: null module source");
			source_ptr opened{ source.release() };
			if (const std::string problem = opened->open(); !problem.empty()) {
				fail(std::format("module_loader: refusing module {}: {}", opened->describe(), problem));
			}
			module_ptr module = create_module(opened->entry(), opened->describe());
			load_entry(make_entry(std::move(opened), std::move(module)), data);
		}

	private:
		static entry make_entry(source_ptr source, module_ptr module) {
			entry e;
			e.source = std::move(source);
			e.module = std::move(module);
			return e;
		}

		static module_ptr create_module(const module_entry& exports, const std::string& origin) {
			if (const std::string problem = check_module_entry(exports); !problem.empty()) {
				fail(std::format("module_loader: refusing {}: {}", origin, problem));
			}
			module_ptr module{ exports.create(), module_deleter{ exports.destroy } };
			if (!module) {
				fail(std::format("module_loader: refusing {}: snk_create_module returned null", origin));
			}
			return module;
		}

		void load_entry(entry e, ds::app_data_storage& data) {
			ASSERT_MSG(e.module, "module_loader::load: null module");
			const std::string name{ e.module->name() };
			if (find(name)) {
				fail(std::format("module_loader: cannot load '{}': a module with this name is already present", name));
			}
			for (const auto dep : e.module->depends_on()) {
				const entry* d = find(dep);
				if (!d || d->state != module_state::initialized) {
					fail(std::format("module_loader: cannot load '{}': dependency '{}' is {}", name, dep,
						d ? state_name(d->state) : std::string_view{ "not loaded" }));
				}
			}

			e.size_before = data.size();
			create_layer(e, data);
			e.module->register_services(storage_of(e, data));
			e.state = module_state::registered;
			try {
				e.module->initialize_services(storage_of(e, data));
			} catch (...) {
				shutdown_entry(e, data);
				if (data.size() != e.size_before) {
					egLOG_ERROR("app", "module '{}' left the service storage asymmetric after a failed load: {} services, expected {}",
						name, data.size(), e.size_before);
				}
				throw;
			}
			e.state = module_state::initialized;
			e.provided = data.size() - e.size_before;

			std::erase(m_unloaded, name);
			m_entries.push_back(std::move(e));
		}

	public:
		// Unloads one initialized module: refused (std::logic_error, nothing touched) if another
		// initialized module depends on it. Otherwise it is shut down and removed. Afterwards the
		// storage must have shrunk by exactly the services the module left behind at load time
		// (for the most recently loaded module that is the size recorded before its registration);
		// a module that forgot to destruct a service is reported with an error log and
		// std::logic_error, after it has been removed, so the leak is visible and not retried.
		void unload(std::string_view requested, ds::app_data_storage& data) {
			const std::string name{ requested }; // the module's own name() dies with it
			entry* e = find(name);
			if (!e) {
				fail(std::format("module_loader: cannot unload '{}': no such module", name));
			}
			if (e->state != module_state::initialized) {
				fail(std::format("module_loader: cannot unload '{}': module is {}", name, state_name(e->state)));
			}
			for (const auto& other : m_entries) {
				if (&other == e || other.state != module_state::initialized) {
					continue;
				}
				for (const auto dep : other.module->depends_on()) {
					if (dep == name) {
						fail(std::format("module_loader: cannot unload '{}': initialized module '{}' depends on it",
							name, other.module->name()));
					}
				}
			}

			const std::size_t expected = data.size() - e->provided;
			shutdown_entry(*e, data);
			const std::size_t actual = data.size();

			m_unloaded.emplace_back(name);
			// The module before its source: erase() move-assigns members in declaration order.
			e->module.reset();
			e->source.reset();
			m_entries.erase(m_entries.begin() + (e - m_entries.data()));

			if (actual != expected) {
				fail(std::format("module_loader: module '{}' left the service storage asymmetric after shutdown: "
					"{} services, expected {}", name, actual, expected));
			}
		}

		// Shuts down only the modules that finished initialize_services, in reverse order.
		void shutdown_all_services(ds::app_data_storage& data) {
			ensure_sorted();
			for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
				if (it->state == module_state::initialized) {
					shutdown_entry(*it, data);
				}
			}
		}
	};

} // namespace app::modules
