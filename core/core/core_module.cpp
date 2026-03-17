#include "core_module.h"
#include "res_resource_service_init.h"
#include "wnd_window_service_init.h"
#include "desc_desc_service_init.h"
#include "ecs_ecs_service_init.h"
#include "wnd_window_system.h"
#include "res_system.h"
#include "adapters/res_adapter_worker.h"
#include <engine_log.h>
#include <cfg_api.h>

namespace {
	CFG_VAR_DEF_STRING(cfg_log_file,  "logger/file",  "logs/engine.log");
	CFG_VAR_DEF_STRING(cfg_log_level, "logger/level", "");

	// Captures all keys under "logger/category_file":
	//   logger/category_file/edt       → logs/editor.log
	//   logger/category_file/resource  → logs/resource.log
	cfg::variable_map cfg_log_category_files{"logger/category_file"};

	// Captures all keys under "logger/category_level":
	//   logger/category_level/edt      → debug
	//   logger/category_level/resource → warn
	cfg::variable_map cfg_log_category_levels{"logger/category_level"};

	engine::log_level parse_log_level(std::string_view str)
	{
		if (str == "trace") return engine::log_level::trace;
		if (str == "debug") return engine::log_level::debug;
		if (str == "info")  return engine::log_level::info;
		if (str == "warn")  return engine::log_level::warn;
		if (str == "error") return engine::log_level::error;
		return engine::log_level::info;
	}
}

void core::core_module::register_services(ds::app_data_storage& data)
{
	res::resource_init(data);// core
	desc::desc_init(data);// core
	window::window_init(data);// core
	ecs::ecs_init(data);// core
}

void core::core_module::initialize_services(ds::app_data_storage& data)
{
	// Apply logger config before other services start logging
	{
		const std::string& file = *cfg_log_file;
		if (!file.empty()) {
			engine::set_log_file(file);
		}

		const std::string& level_str = *cfg_log_level;
		if (!level_str.empty()) {
			engine::set_log_level(parse_log_level(level_str));
		}

		// Per-category file sinks
		for (const auto& [category, path] : cfg_log_category_files.entries()) {
			engine::set_category_file(category, path);
		}

		// Per-category log levels
		for (const auto& [category, level_str] : cfg_log_category_levels.entries()) {
			engine::set_category_level(category, parse_log_level(level_str));
		}
	}

	// Initialize core services here
	auto& window = data.require<wnd::window_system>();
	window.init();
	auto& resource = data.require<::res::resource_system>();
	resource.set_adapter_worker(std::make_unique<core::res::async_adapter_worker>());
}

void core::core_module::shutdown_services(ds::app_data_storage& data)
{
	ecs::ecs_term(data);
	window::window_term(data);
	desc::desc_term(data);
	res::resource_term(data);
}
