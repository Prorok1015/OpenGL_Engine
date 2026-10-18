#pragma once
#include "app_module_interface.h"
#include "engine_assert.h"
#include "engine_log.h"
#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
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
		struct entry {
			std::unique_ptr<module_interface> module;
			module_state state = module_state::added;
			std::size_t size_before = 0; // storage.size() right before this module registered
			std::size_t provided = 0;    // services it left in the storage after register + initialize
		};

		std::vector<entry> m_entries;
		std::vector<std::string> m_unloaded; // names; the module objects are gone
		bool m_sorted = false;

		void shutdown_entry(entry& e, ds::app_data_storage& data) {
			e.state = module_state::shut_down;
			try {
				e.module->shutdown_services(data);
			} catch (const std::exception& ex) {
				egLOG_ERROR("app", "module '{}' threw during shutdown: {}", e.module->name(), ex.what());
			} catch (...) {
				egLOG_ERROR("app", "module '{}' threw unknown exception during shutdown", e.module->name());
			}
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
			m_entries.push_back(entry{ std::move(module) });
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
				e.module->register_services(data);
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
					}
					e.module->initialize_services(data);
					e.state = module_state::initialized;
					e.provided = data.size() - e.size_before;
					done.push_back(i);
				}
			} catch (...) {
				shutdown_entries(done, data);
				throw;
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
