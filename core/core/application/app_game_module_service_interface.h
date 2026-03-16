#pragma once
#include "ds_store.hpp"

namespace app::modules {

    class game_module_service_interface {
    public:
        virtual ~game_module_service_interface() = default;
        virtual bool load_game_runtime(ds::app_data_storage& data) = 0;
        virtual void unload_game_runtime(ds::app_data_storage& data) = 0;
        virtual bool is_running() const = 0;
    };

} // namespace app::modules