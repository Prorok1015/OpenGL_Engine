#pragma once
#include "ds/ds_store.hpp"
#include <string_view>
#include <vector>

namespace app::modules {
	struct module_interface
	{
		virtual ~module_interface() = default;

		virtual void register_services(ds::app_data_storage& data) = 0;
		virtual void initialize_services(ds::app_data_storage& data) = 0;
		virtual void shutdown_services(ds::app_data_storage& data) = 0;

		// Stable, unique identifier of this module (EPIC-49 US-49-7). Used to express dependencies.
		virtual std::string_view name() const = 0;
		// Names of modules that must be registered/initialized before this one. The loader
		// stable-topo-sorts on these; independent modules keep their insertion order.
		virtual std::vector<std::string_view> depends_on() const { return {}; }
	};
}