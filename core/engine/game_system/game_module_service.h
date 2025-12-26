#pragma once
#include "app_module_service_interface.h"
#include "game_module.h"
#include "resource_system.h"
#include <iostream> 

namespace modules {

    class GameModuleService : public IGameModuleService {
    private:
        std::unique_ptr<GameModule> m_game_module = nullptr;

    public:
        bool load_game_runtime(ds::AppDataStorage& data) override {
            if (m_game_module) {
                std::cout << "Game runtime is already running." << std::endl;
                return true;
            }

            try {
                res::resource_system& rs = data.require<res::resource_system>();
                std::cout << "GameModuleService: Found global ResourceSystem at address " << &rs << std::endl;
            }
            catch (const std::exception& e) {
                std::cerr << "ERROR: Cannot start game runtime. Missing Core service: " << e.what() << std::endl;
                return false;
            }

            m_game_module = std::make_unique<GameModule>();
            m_game_module->register_services(data);
            m_game_module->initialize_services(data);

            std::cout << "Game runtime loaded successfully." << std::endl;
            return true;
        }

        void unload_game_runtime(ds::AppDataStorage& data) override {
            if (!m_game_module) return;

            std::cout << "Unloading game runtime..." << std::endl;
            m_game_module->shutdown_services(data);

            m_game_module = nullptr;
            std::cout << "Game runtime unloaded." << std::endl;
        }

        bool is_running() const override {
            return m_game_module != nullptr;
        }
    };

    void register_game_module_service(ds::AppDataStorage& data) {
        data.register_service<IGameModuleService>(std::make_shared<GameModuleService>());
    }

} // namespace modules