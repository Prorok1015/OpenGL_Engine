#include "edt_game_build_component.h"
#include "edt_build_jobs.h"
#include "edt_build_settings_model.h"
#include "edt_build_settings_panel.h"
#include "edt_editor_layer.h"
#include "edt_module_swap_gate.h"
#include "edt_play_session.h"
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_variables.h>
#include <snk/core/core_project_modules.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/platform/plt_shell_open.h>
#include <snk/project/prj_project.h>
#include <format>
#include <utility>

namespace
{
	// Build > Build Game (#434). Empty paths: found by the editor (see game_build_sdk_dir, edt_toolchain.h).
	CFG_VAR_DEF_PATH(cfg_build_sdk_dir, "build/sdk_dir", "");
	CFG_VAR_DEF_PATH(cfg_build_cmake, "build/cmake", "");
	CFG_VAR_DEF_PATH(cfg_build_vs_path, "build/vs_path", "");
	CFG_VAR_DEF_STRING(cfg_build_vs_min_version, "build/vs_min_version", "17.14");

	// The SDK the game module is built against: build/sdk_dir, else the installed editor's, else the engine build
	// tree's (where this editor was built).
	std::filesystem::path game_build_sdk_dir()
	{
		if (!cfg_build_sdk_dir->empty()) return *cfg_build_sdk_dir;
		const std::filesystem::path program = platform::current_executable();
		if (!program.empty()) {
			const auto installed = (program.parent_path() / ".." / "lib" / "cmake" / "snake_sdk").lexically_normal();
			std::error_code ec;
			if (std::filesystem::is_regular_file(installed / "snake_sdkConfig.cmake", ec)) return installed;
		}
#ifdef SNK_BUILD_TREE_SDK_DIR
		return std::filesystem::path(std::u8string_view{ SNK_BUILD_TREE_SDK_DIR });
#else
		return {};
#endif
	}
}

edt::game_build_component::game_build_component(ports from)
	: m_ports(std::move(from))
{
	game_builder_options build_options;
	if (m_ports.project != nullptr) {
		const prj::project_context& project = *m_ports.project;
		game_project game{ project.root, project_module_names(project) };
		build_options.project = [game] { return std::optional<game_project>{ game }; };
		build_options.settings = [snk_dir = project.snk_dir()] { return load_build_settings(snk_dir); };
		build_options.preset_for = [code_dir = project.root / "code"](std::string_view configuration) {
			return preset_for_configuration(code_dir, configuration);
		};
		build_options.on_fresh_configured = [snk_dir = project.snk_dir()](std::string_view configuration, std::uint64_t generation) {
			if (auto problem = clear_fresh_configure(snk_dir, configuration, generation)) {
				egLOG_WARN("editor/build", "{}: the next build of {} configures with --fresh again", *problem, configuration);
			}
		};
	} else {
		build_options.project = [] { return std::optional<game_project>{}; };
	}
	build_options.sdk_dir = game_build_sdk_dir();
	build_options.default_configuration = m_ports.default_configuration;
	build_options.toolchain = { *cfg_build_cmake, *cfg_build_vs_path, *cfg_build_vs_min_version };
	build_options.show_progress = [this](std::string_view id, std::function<bool()> draw) {
		m_ports.layer.register_implicit(id, std::move(draw));
	};
	build_options.events = &m_ports.context.events();
	build_options.auto_jobs = [] { return estimate_jobs().jobs; };
	build_options.start_blocker = m_ports.migration_blocker;
	build_options.trusted = [this] {
		const auto trust = m_ports.trust.lock();
		return trust && trust->trusted();
	};
	build_options.ask_trust = m_ports.ask_trust;
	build_options.own_build = [this](std::string_view module, std::string_view digest) {
		if (const auto trust = m_ports.trust.lock()) trust->record_own_build_digest(module, digest);
	};
	m_builder = std::make_unique<game_builder>(m_ports.context.jobs(), m_ports.context.problems(), m_ports.context.dialogs(),
		m_ports.context.actions(), std::move(build_options));

	game_reloader_options reload_options;
	reload_options.modules = [project = m_ports.project] {
		return project != nullptr ? project_module_names(*project) : std::vector<std::string>{};
	};
	reload_options.is_editor_module = [host = m_ports.module_host](const std::string& module) {
		core::project_module_host* modules = host();
		return modules != nullptr && modules->is_editor_module(module);
	};
	reload_options.is_current = [host = m_ports.module_host](const std::string& module) {
		core::project_module_host* modules = host();
		return modules != nullptr && modules->is_current(module);
	};
	// The action may be asked for from a panel of an editor module: whatever unloads editor modules waits for the next UI frame.
	reload_options.unloads_editor_modules = [host = m_ports.module_host](const std::string& module) {
		core::project_module_host* modules = host();
		if (modules == nullptr) return false;
		return modules->is_editor_module(module) || modules->has_editor_modules();
	};
	reload_options.build_output = [host = m_ports.module_host](const std::string& module) -> std::optional<std::filesystem::path> {
		core::project_module_host* modules = host();
		if (modules == nullptr) return std::nullopt;
		return modules->build_output(module);
	};
	const auto reload_module = [this](const std::string& module, bool retry_taken_back) {
		core::project_module_host* modules = m_ports.module_host();
		if (modules != nullptr && modules->is_editor_module(module)) {
			// An editor module changes neither the game layer nor Play: no swap gate, no snapshot, nothing to revert.
			return modules->reload_editor_module(module);
		}
		core::reload_report report = modules != nullptr
			? modules->reload(module, m_ports.swap_gate.hooks(), retry_taken_back)
			: core::reload_report{ core::reload_status::refused, "the editor did not load the project's modules" };
		play_session* play = m_ports.play();
		// A snapshot before_reload took and no swap used goes, whatever the outcome.
		if (play) play->end_reload();
		// The build loaded but could not restore Play: it is taken back next frame, so it is not reported as running.
		if (report.status == core::reload_status::reloaded && play && play->reverting()) {
			report.status = core::reload_status::rolled_back;
			report.message = std::format("it could not restore Play ({}); it is being taken back, the previous build loads again",
				play->restore_failure());
		}
		return report;
	};
	reload_options.reload = [reload_module](const std::string& module) { return reload_module(module, false); };
	// Only a person loads a build the host took back again (#601): live mode would loop.
	reload_options.reload_by_person = [reload_module](const std::string& module) { return reload_module(module, true); };
	// Play keeps its world across the swap (#600): the snapshot is taken after every check, right before it.
	reload_options.before_reload = [this](const std::string&) {
		play_session* play = m_ports.play();
		return play ? play->capture_for_reload() : std::string{};
	};
	reload_options.building = [this] { return m_builder->is_manual_build_running(); };
	reload_options.background_building = [this] { return m_builder->building_preemptible(); };
	reload_options.stop_background = [this] {
		// Not cancel(): that drops the person's request waiting for the live build and gives live mode no outcome.
		m_builder->stop_background();
	};
	reload_options.events = &m_ports.context.events();
	reload_options.trusted = [this] {
		const auto trust = m_ports.trust.lock();
		return trust && trust->trusted();
	};
	reload_options.ask_trust = m_ports.ask_trust;
	reload_options.next_frame = [this](const std::string& id, std::function<void()> fn) {
		m_ports.layer.register_implicit(id, [fn = std::move(fn)] {
			fn();
			return false;
		});
	};
	// Reload during a live build: waits under edt/reload_game_module/<module>, which also holds live mode's next build.
	reload_options.each_frame = [this](const std::string& id, std::function<bool()> poll) {
		m_ports.layer.register_implicit(id, std::move(poll));
	};
	reload_options.defer = [this] {
		if (auto migrating = m_ports.migration_blocker()) return *migrating;
		if (auto loading = m_ports.load_in_progress()) return *loading;
		if (m_ports.level_switch_pending()) return std::string{ "the game is switching to another level" };
		// A second reload waits for the first one's restore (#600): its snapshot would see the set held in Pause.
		if (play_session* play = m_ports.play(); play && play->reloading()) {
			return std::string{ "Play is loading the resources of the last reload" };
		}
		// The swap closes the prefab editor: its unapplied edits must not go with it.
		if (m_ports.prefab_has_unapplied_edits()) return std::string{ "the prefab editor has unapplied edits: apply or revert them first" };
		return std::string{};
	};
	m_reloader = std::make_unique<game_reloader>(m_ports.context.dialogs(), m_ports.context.actions(), std::move(reload_options));
}

edt::game_build_component::~game_build_component() = default;

void edt::game_build_component::install(std::vector<registration>& out)
{
	out.push_back(m_ports.context.actions().add({ .id = "build.settings.open", .label = "Build Settings...",
		.menu_path = "Build", .separator_before = true,
		.execute = no_args([this] {
			m_ports.context.panels().set_open(m_ports.settings_panel.get_id(), true);
			m_ports.settings_panel.focus();
		}),
		.expose_as_tool = false, .category = cmd::agent_category::not_for_agents }));
	build_settings_command_options settings_command;
	settings_command.project_root = [project = m_ports.project]() -> std::optional<std::filesystem::path> {
		if (project == nullptr) return std::nullopt;
		return project->root;
	};
	settings_command.default_configuration = m_ports.default_configuration;
	settings_command.live_enabled = m_ports.live_mode_enabled;
	for (auto& command : register_build_settings_commands(m_ports.context.actions().commands(), std::move(settings_command))) {
		out.push_back(std::move(command));
	}
}
