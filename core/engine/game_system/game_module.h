#pragma once
#include "app_module_interface.h"

namespace game
{
	using MODULE_PRIORITY = app::modules::MODULE_PRIORITY;
	class game_module : public app::modules::module_interface
	{
	public:
		virtual void register_services(ds::app_data_storage& data) override;
		virtual void initialize_services(ds::app_data_storage& data) override;
		virtual void shutdown_services(ds::app_data_storage& data) override;
		virtual MODULE_PRIORITY get_priority() const override {
			return MODULE_PRIORITY::GAME;
		}
	};
}