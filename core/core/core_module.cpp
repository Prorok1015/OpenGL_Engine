#include <snk/core/core_module.h>
#include <snk/resource/res_resource_service_init.h>
#include <snk/window/wnd_window_service_init.h>
#include <snk/desc/desc_desc_service_init.h>
#include <snk/ecs/ecs_ecs_service_init.h>
#include <snk/window/wnd_window_system.h>
#include <snk/application/app_loop_service.h>
#include <snk/resource/res_system.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_api.h>

#include <algorithm>
#include <chrono>

namespace {
	CFG_VAR_DEF_STRING(cfg_log_file,  "logger/file",  "logs/engine.log");
	CFG_VAR_DEF_STRING(cfg_log_level, "logger/level", "");

	// Main-thread queue limits (ds::task_system, `tasks` frame stage).
	CFG_VAR_DEF_FLOAT(cfg_tasks_main_budget_ms, "tasks/main_budget_ms", 2.0f);
	CFG_VAR_DEF_INT(cfg_tasks_gpu_upload_per_frame, "tasks/gpu_upload_per_frame", 4);

	// Headless determinism (tools/run_headless.sh --frames): constant frame dt instead of wall clock.
	CFG_VAR_DEF_FLOAT(cfg_fixed_frame_dt, "app/fixed_frame_dt", 0.0f);
	CFG_VAR_DEF_INT(cfg_freeze_after_frames, "app/freeze_after_frames", 0);

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
	// Before resource_system (and so destroyed after it): resource work will run on its lanes.
	data.construct<ds::task_system>();
	res::resource_init(data);// core
	desc::desc_init(data);// core
	window::window_init(data);// core
	ecs::ecs_init(data);// core
	// The frame loop is shared by every host; modules add their stages to the registry.
	auto& stages = data.construct<app::frame_stage_registry>();
	app::register_core_frame_stages(stages);
	stages.add_stage("tasks", app::builtin_stage::input, app::stage_side::after, [](app::stage_context& ctx) {
		ctx.storage.require<ds::task_system>().drain_main();
	});
	const app::deterministic_time time{ std::max(0.f, static_cast<float>(cfg_fixed_frame_dt)), std::max(0, static_cast<int>(cfg_freeze_after_frames)) };
	// Kept in storage so the GUI layer can feed ImGui the same dt.
	data.construct<app::deterministic_time>(time);
	auto& loop = data.construct<app::app_loop_service_interface, app::loop_service>(stages, time);
	if (time.enabled()) {
		static_cast<app::loop_service&>(loop).set_settled_probe([](ds::app_data_storage& storage) {
			auto& tasks = storage.require<ds::task_system>();
			return storage.require<::res::resource_system>().pending_loads() == 0
				&& tasks.pending_main(ds::main_class::general) == 0
				&& tasks.pending_main(ds::main_class::gpu_upload) == 0;
		});
	}
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

	auto& tasks = data.require<ds::task_system>();
	tasks.set_main_budget(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::duration<float, std::milli>(std::max(0.f, static_cast<float>(cfg_tasks_main_budget_ms)))));
	tasks.set_gpu_upload_limit(static_cast<std::size_t>(std::max(0, static_cast<int>(cfg_tasks_gpu_upload_per_frame))));

	// Initialize core services here
	auto& window = data.require<wnd::window_system>();
	window.init();
}

void core::core_module::shutdown_services(ds::app_data_storage& data)
{
	data.destruct<app::app_loop_service_interface>();
	data.destruct<app::frame_stage_registry>();
	ecs::ecs_term(data);
	window::window_term(data);
	desc::desc_term(data);
	res::resource_term(data);
	data.destruct<ds::task_system>();
}
