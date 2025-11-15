#pragma once
#include "eng_module_interface.h"
#include <vector>
#include <memory>
#include <ranges>

namespace modules {

    class module_loader {
    private:
        std::vector<std::unique_ptr<module_interface>> m_modules;

    public:
        void add_module(std::unique_ptr<module_interface> module) {
            m_modules.push_back(std::move(module));
        }

        void register_all_services(ds::app_data_storage& data) {
            for (auto& mod : m_modules) {
                mod->register_services(data);
            }
        }

        void initialize_all_services(ds::app_data_storage& data) {
            //std::sort(m_modules.begin(), m_modules.end(),
            //    [](const auto& a, const auto& b) {
            //        return a->get_priority() < b->get_priority();
            //    });
            std::ranges::sort(m_modules, {}, [](const auto& m) {
                return m->get_priority();
                });

            for (auto& mod : m_modules) {
                mod->initialize_services(data);
            }
        }

        void shutdown_all_services(ds::app_data_storage& data) {
            for (auto& module : m_modules | std::ranges::views::reverse) {
                module->shutdown_services(data);
            }
        }
    };

} // namespace modules