#pragma once
#include "ds/ds_store.hpp"
#include <string_view>
#include <vector>

namespace app::modules {
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

		// Stable, unique identifier of this module (EPIC-49 US-49-7). Used to express dependencies.
		virtual std::string_view name() const = 0;
		// Names of modules that must be registered/initialized before this one. The loader
		// topo-sorts on these; get_priority() is only a tiebreaker between independent modules.
		virtual std::vector<std::string_view> depends_on() const { return {}; }
	};
}