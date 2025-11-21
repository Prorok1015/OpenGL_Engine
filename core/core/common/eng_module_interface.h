#pragma once
#include "ds_store.hpp"

namespace modules {
	enum class MODULE_PRIORITY
	{
		CORE,
		ENGINE,
		EDITOR,
		GAME
	};
	
	struct module_interface
	{
		virtual ~module_interface() = default;

		virtual void register_services(ds::app_data_storage& data) = 0;
		virtual void initialize_services(ds::app_data_storage& data) = 0;
		virtual void shutdown_services(ds::app_data_storage& data) = 0;

		virtual MODULE_PRIORITY get_priority() const = 0;
	};
}