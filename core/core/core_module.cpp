#include <snk/core/core_module.h>
#include <snk/resource/res_resource_service_init.h>
#include <snk/window/wnd_window_service_init.h>
#include <snk/desc/desc_desc_service_init.h>
#include <snk/ecs/ecs_ecs_service_init.h>
#include <snk/window/wnd_window_system.h>
#include <snk/application/app_frame_commands.h>
#include <snk/application/app_frame_control.h>
#include <snk/application/app_loop_service.h>
#include <snk/resource/res_system.h>
#include <snk/command/cmd_log_commands.h>
#include <snk/command/cmd_registry.h>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_variables.h>
#include <snk/project/prj_project.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

namespace {
	CFG_VAR_DEF_STRING(cfg_log_file,  "logger/file",  "logs/engine.log");
	CFG_VAR_DEF_STRING(cfg_log_level, "logger/level", "");
	// How many of the newest log messages the journal keeps (log.read); 0 turns it off.
	CFG_VAR_DEF_INT(cfg_log_journal_capacity, "logger/journal_capacity", 10000);
	// Lowest level the journal keeps (trace|debug|info|warn|error); empty: whatever the logger's own level lets through.
	CFG_VAR_DEF_STRING(cfg_log_journal_min_level, "logger/journal_min_level", "");

	// Main-thread queue limits (ds::task_system, `tasks` frame stage).
	CFG_VAR_DEF_FLOAT(cfg_tasks_main_budget_ms, "tasks/main_budget_ms", 2.0f);
	CFG_VAR_DEF_INT(cfg_tasks_gpu_upload_per_frame, "tasks/gpu_upload_per_frame", 4);

	// Headless determinism (tools/run_headless.sh --frames): constant frame dt instead of wall clock.
	CFG_VAR_DEF_FLOAT(cfg_fixed_frame_dt, "app/fixed_frame_dt", 0.0f);
	CFG_VAR_DEF_INT(cfg_freeze_after_frames, "app/freeze_after_frames", 0);
	// > 0: the loop stops after this many frames and the host exits with 0 (CI smoke runs of --headless).
	CFG_VAR_DEF_INT(cfg_exit_after_frames, "app/exit_after_frames", 0);

	// --project <project.snkproj or its directory>; empty: no project, res:// comes from resource/path.
	CFG_VAR_DEF_PATH(cfg_project_file, "project/file", "");
	// --allow-engine-version-mismatch: open a project of an older engine MAJOR.MINOR without asking (never a newer one).
	CFG_VAR_DEF_BOOL(cfg_allow_engine_version_mismatch, "project/allow_engine_version_mismatch", false);
	// --non-interactive / -n: nobody can answer a question, so none is asked and each takes its safe answer. Today
	// only the engine version check asks (prj::refuse_without_asking).
	CFG_VAR_DEF_BOOL(cfg_non_interactive, "app/non_interactive", false);
	// --headless: there is no window to pick a project in, so a run without --project is a project error.
	CFG_VAR_DEF_BOOL(cfg_project_required, "project/required", false);

	// Captures all keys under "logger/category_file":
	//   logger/category_file/edt       → logs/editor.log
	//   logger/category_file/resource  → logs/resource.log
	cfg::variable_map cfg_log_category_files{"logger/category_file"};

	// Captures all keys under "logger/category_level":
	//   logger/category_level/edt      → debug
	//   logger/category_level/resource → warn
	cfg::variable_map cfg_log_category_levels{"logger/category_level"};

	// Commands the engine itself offers in every process; they leave the registry with this holder.
	struct engine_commands
	{
		cmd::registration log;
		app::frame_command_registrations app_frame;
	};

	std::optional<engine::log_level> parse_known_log_level(std::string_view str)
	{
		if (str == "trace") return engine::log_level::trace;
		if (str == "debug") return engine::log_level::debug;
		if (str == "info") return engine::log_level::info;
		if (str == "warn") return engine::log_level::warn;
		if (str == "error") return engine::log_level::error;
		return std::nullopt;
	}

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

namespace {
	// Before resource_init, which mounts the project's content roots.
	void open_cli_project(ds::app_data_storage& data)
	{
		const std::filesystem::path file = *cfg_project_file;
		if (file.empty()) {
			if (cfg_project_required) {
				throw prj::project_error("no project given: --headless needs --project <project.snkproj or its directory>");
			}
			return;
		}
		auto opened = prj::open_project(file);
		if (!opened.context) {
			throw prj::project_error(opened.error);
		}
		auto& project = *opened.context;

		const auto action = prj::version_action_for(project.engine_match, cfg_allow_engine_version_mismatch);
		const auto engine_line = std::format("project '{}' was made for engine {}, this is engine {}", project.data.name,
			prj::to_string(project.data.engine), prj::to_string(prj::current_engine_version()));
		if (prj::refuse_without_asking(action, cfg_non_interactive)) {
			throw prj::engine_version_error(std::format("{}; not opened: a non-interactive run does not ask{}", engine_line,
				action == prj::version_action::ask_open_or_quit ? " (start with --allow-engine-version-mismatch to open it)" : ""));
		}
		if (action == prj::version_action::ask_open_or_quit || action == prj::version_action::refuse) {
			// Not mounted: the host runs without a project and asks (or tells) the user; see prj::held_project.
			egLOG_WARN("project", "{}; not opened{}", engine_line, action == prj::version_action::ask_open_or_quit
				? " (start with --allow-engine-version-mismatch to open it without migrating)" : "");
			data.construct<prj::held_project>(prj::held_project{ project.manifest_file, project.data.name, project.data.engine,
				action, prj::held_project::decision::none });
			return;
		}
		if (action == prj::version_action::open_with_notice) {
			egLOG_INFO("project", "{}", engine_line);
		}
		egLOG_INFO("project", "opened project '{}' ({})", project.data.name, project.manifest_file.string());
		data.construct<prj::project_context>(std::move(project));
	}
}

void core::core_module::register_services(ds::app_data_storage& data)
{
	// Before resource_system (and so destroyed after it): resource work will run on its lanes.
	data.construct<ds::task_system>();
	// One command registry per process: the engine registers into it here, the editor and the game add theirs.
	// Created on the main thread, which is the only one that may invoke commands.
	auto& commands = data.construct<cmd::command_registry>();
	data.construct<engine_commands>().log = cmd::register_log_commands(commands);
	open_cli_project(data);
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
	// The frame loop steps by it on `app.advance`; destroyed after the commands that hold a reference to it.
	auto& frame_control = data.construct<app::frame_control>(time);
	data.require<engine_commands>().app_frame = app::register_frame_commands(commands, frame_control);
	auto& loop = data.construct<app::app_loop_service_interface, app::loop_service>(stages, time, &frame_control);
	auto* loop_impl = dynamic_cast<app::loop_service*>(&loop);
	ASSERT_MSG(loop_impl, "core_module: the frame loop is not app::loop_service");
	if (loop_impl) {
		loop_impl->set_exit_after_frames(static_cast<std::uint64_t>(std::max(0, static_cast<int>(cfg_exit_after_frames))));
	}
	if (time.enabled()) {
		if (loop_impl) loop_impl->set_settled_probe([](ds::app_data_storage& storage) {
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

		const int journal_capacity = static_cast<int>(cfg_log_journal_capacity);
		engine::set_log_journal_capacity(static_cast<std::size_t>(std::max(0, journal_capacity)));

		const std::string& journal_level = *cfg_log_journal_min_level;
		if (!journal_level.empty()) {
			if (const auto parsed = parse_known_log_level(journal_level)) {
				engine::set_log_journal_min_level(*parsed);
			} else {
				egLOG_WARN("logger", "logger/journal_min_level '{}' is not trace, debug, info, warn or error: the journal follows the logger's level", journal_level);
			}
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
	// The loop holds a pointer to the frame_control: it goes first, so nothing can step with a dead one.
	data.destruct<app::app_loop_service_interface>();
	data.destruct<app::frame_stage_registry>();
	ecs::ecs_term(data);
	window::window_term(data);
	desc::desc_term(data);
	res::resource_term(data);
	if (data.has_value<prj::project_context>()) {
		data.destruct<prj::project_context>();
	}
	if (data.has_value<prj::held_project>()) {
		data.destruct<prj::held_project>();
	}
	data.destruct<engine_commands>();
	data.destruct<app::frame_control>();
	data.destruct<cmd::command_registry>();
	data.destruct<ds::task_system>();
}
