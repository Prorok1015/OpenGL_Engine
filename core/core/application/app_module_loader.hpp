#pragma once
#include "app_module_interface.h"
#include "engine_assert.h"
#include "engine_log.h"
#include <exception>
#include <utility>
#include <vector>
#include <memory>
#include <ranges>
#include <algorithm>
#include <string_view>
#include <unordered_map>

namespace app::modules {

    class module_loader {
    private:
        std::vector<std::unique_ptr<module_interface>> m_modules;
        bool m_sorted = false;
        std::size_t m_initialized = 0;

        void shutdown_initialized(ds::app_data_storage& data) {
            const std::size_t count = std::exchange(m_initialized, 0);
            for (std::size_t i = count; i-- > 0;) {
                try {
                    m_modules[i]->shutdown_services(data);
                } catch (const std::exception& e) {
                    egLOG_ERROR("app", "module '{}' threw during shutdown: {}", m_modules[i]->name(), e.what());
                } catch (...) {
                    egLOG_ERROR("app", "module '{}' threw unknown exception during shutdown", m_modules[i]->name());
                }
            }
        }

        // Topo-sort modules by their declared depends_on() once, so register, initialize and
        // shutdown all use one consistent order (EPIC-49 US-49-7). Dependencies come first;
        // get_priority() (then insertion order) only breaks ties between independent modules.
        void ensure_sorted() {
            if (m_sorted) {
                return;
            }
            const std::size_t n = m_modules.size();
            std::unordered_map<std::string_view, std::size_t> index_of;
            for (std::size_t i = 0; i < n; ++i) {
                const auto [it, inserted] = index_of.emplace(m_modules[i]->name(), i);
                ASSERT_MSG(inserted, "module_loader: duplicate module name");
            }

            std::vector<int> in_degree(n, 0);
            std::vector<std::vector<std::size_t>> dependents(n);
            for (std::size_t i = 0; i < n; ++i) {
                for (const auto dep : m_modules[i]->depends_on()) {
                    const auto it = index_of.find(dep);
                    ASSERT_MSG(it != index_of.end(), "module_loader: dependency on unknown module");
                    dependents[it->second].push_back(i);
                    ++in_degree[i];
                }
            }

            // Among modules with no remaining dependency, pick lowest priority then earliest insertion.
            const auto tiebreak = [this](std::size_t a, std::size_t b) {
                const auto pa = m_modules[a]->get_priority();
                const auto pb = m_modules[b]->get_priority();
                if (pa != pb) {
                    return pa < pb;
                }
                return a < b;
            };

            std::vector<std::size_t> ready;
            for (std::size_t i = 0; i < n; ++i) {
                if (in_degree[i] == 0) {
                    ready.push_back(i);
                }
            }

            std::vector<std::size_t> order;
            order.reserve(n);
            while (!ready.empty()) {
                const auto best = std::min_element(ready.begin(), ready.end(), tiebreak);
                const std::size_t cur = *best;
                ready.erase(best);
                order.push_back(cur);
                for (const std::size_t d : dependents[cur]) {
                    if (--in_degree[d] == 0) {
                        ready.push_back(d);
                    }
                }
            }
            ASSERT_MSG(order.size() == n, "module_loader: dependency cycle detected");

            std::vector<std::unique_ptr<module_interface>> sorted;
            sorted.reserve(n);
            for (const std::size_t idx : order) {
                sorted.push_back(std::move(m_modules[idx]));
            }
            m_modules = std::move(sorted);
            m_sorted = true;
        }

    public:
        void add_module(std::unique_ptr<module_interface> module) {
            m_modules.push_back(std::move(module));
            m_sorted = false;
        }

        void register_all_services(ds::app_data_storage& data) {
            ensure_sorted();
            for (auto& mod : m_modules) {
                mod->register_services(data);
            }
        }

        // If a module throws, the modules initialized before it are shut down in reverse order
        // and the exception is rethrown. The throwing module itself is not shut down: it never
        // finished initializing and is expected to clean up after itself.
        void initialize_all_services(ds::app_data_storage& data) {
            ensure_sorted();
            m_initialized = 0;
            try {
                for (auto& mod : m_modules) {
                    mod->initialize_services(data);
                    ++m_initialized;
                }
            } catch (...) {
                shutdown_initialized(data);
                throw;
            }
        }

        // Shuts down only the modules that finished initialize_services, in reverse order.
        void shutdown_all_services(ds::app_data_storage& data) {
            ensure_sorted();
            shutdown_initialized(data);
        }
    };

} // namespace modules
