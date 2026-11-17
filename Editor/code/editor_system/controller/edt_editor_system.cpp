#include "edt_editor_system.h"
#include "edt_agent_log_filter.h"
#include "edt_level_load_component.h"
#include "edt_asset_component.h"
#include "edt_asset_open_component.h"
#include "edt_asset_database.h"
#include "edt_asset_index.h"
#include "edt_asset_picker.h"
#include "edt_document_creator.h"
#include "edt_asset_problems.h"
#include "edt_asset_usages.h"
#include "edt_startup_component.h"
#include "edt_game_layer_component.h"
#include <memory>
#include <algorithm>
#include <chrono>
#include <snk/editor/edt_editor_context.h>
#include <snk/common/engine_assert.h>
#include <snk/common/fs_paths.h>
#include <snk/common/text_escape.h>
#include "edt_selection_wiring.h"
#include "edt_prefab_document.h"
#include "edt_prefab_editor_component.h"
#include "edt_editor_camera.h"
#include <snk/editor/edt_document_manager.h>

#include "edt_editor_layer.h"
#include "edt_scene_hierarchy_panel.h"
#include "edt_inspector_panel.h"
#include "edt_override_edit.h"
#include "edt_viewport_panel.h"
#include "edt_game_view_panel.h"
#include "edt_console_panel.h"
#include "edt_panels_component.h"
#include "edt_problems_component.h"
#include "edt_problems_panel.h"
#include "edt_blackboard_panel.h"
#include "edt_input_actions_panel.h"
#include "edt_asset_browser_panel.h"
#include "edt_code_section.h"
#include <snk/platform/plt_shell_open.h>
#include "edt_data_document_host.h"
#include "edt_component_renderers.h"
#include "edt_builtin_asset_kinds.h"
#include "edt_import_component.h"
#include "edt_input_target.h"
#include "edt_edit_menu.h"
#include "edt_file_menu_component.h"
#include "edt_template_ids.h"
#include "edt_scene_commands.h"
#include "edt_code_commands.h"
#include "edt_desc_file_commands.h"
#include "edt_migration_commands.h"
#include "edt_migration_component.h"
#include "edt_migration_window_model.h"
#include "edt_migration_panel.h"
#include "edt_migration_startup.h"
#include "edt_game_build_component.h"
#include "edt_game_builder.h"
#include "edt_game_reloader.h"
#include "edt_build_jobs.h"
#include "edt_build_settings_model.h"
#include "edt_build_settings_panel.h"
#include "edt_git_panel.h"
#include "edt_world_systems_panel.h"
#include "edt_vcs_component.h"
#include "edt_live_mode_component.h"
#include "edt_live_consent.h"
#include "edt_project_trust_service.h"
#include "edt_agent_policy.h"
#include "edt_settings.h"
#include "edt_play_component.h"
#include "edt_play_session.h"
#include "edt_editor_mode.h"
#include <snk/core/core_project_modules.h>
#include "edt_main_menu.h"
#include "edt_agent_access.h"
#include "edt_agent_model_guard.h"
#include "edt_agent_server.h"
#include "edt_agent_server_settings.h"
#include "edt_agent_component.h"
#include "edt_agent_ui_component.h"
#include "edt_editor_quit.h"
#include "edt_data_protocol.h"
#include "edt_engine_version_prompt.h"
#include "edt_recent_projects.h"
#include "edt_document_cast.h"
#include "edt_state_commands.h"
#include <snk/application/app_deterministic_time.h>
#include <snk/application/app_loop_service_interface.h>
#include <snk/command/cmd_registry.h>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include <snk/scene/level/scn_start_level.h>
#include <snk/project/prj_create.h>
#include <snk/project/prj_json_text.h>
#include <snk/project/prj_project.h>
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_asset_steps.h>
#include <snk/project_migration/pmg_assets.h>
#include <snk/platform/plt_process.h>
#include <snk/platform/plt_stdio.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/render/rnd_capture_service.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/gui/gui_system.h>
#include <snk/input/inp_input_router.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_world.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/window/wnd_window_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/config/cfg_variables.h>
#include <imgui.h>
#include <snk/ecs/ecs_system.h>
#include <entt/entt.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// --headless (edt_editor_init.cpp).
CFG_VAR_EXT_BOOL(cfg_edt_headless);
// --trust-project (#921), also read by main.cpp: known here before live mode at open is decided.
CFG_VAR_DEF_BOOL(cfg_edt_trust_project, "project/trust", false);

namespace {
	// The configuration files the editor read its settings from (--config, else the one beside the executable): an agent's
	// checkout or pull must not change them, import/decoder_sandbox is a person's (#1076).
	std::vector<std::filesystem::path> editor_config_files()
	{
		std::vector<std::filesystem::path> files;
		const auto add = [&files](const std::filesystem::path& file) {
			if (file.empty()) return;
			std::error_code ec;
			std::filesystem::path absolute = std::filesystem::weakly_canonical(file, ec);
			if (ec) absolute = std::filesystem::absolute(file, ec);
			if (ec || absolute.empty()) return;
			if (std::find(files.begin(), files.end(), absolute) == files.end()) files.push_back(std::move(absolute));
		};
		add(cfg::loaded_config_path());
		if (const std::filesystem::path program = platform::current_executable(); !program.empty()) {
			add(program.parent_path() / ".." / "config" / "editor.cfg");
		}
		return files;
	}


	// Play locks the document: what would add to the scene is refused where the person sees it, not only in the log.
	bool refused_during_play(const edt::play_session* play, edt::dialog_service& dialogs, std::string text)
	{
		if (!play || !play->active()) return false;
		egLOG_WARN("play", "{}", text);
		dialogs.message(edt::message_level::warning, std::move(text));
		return true;
	}

	// The run ends with 0 once the first loaded level settled (#669; CI: the headless run proves the level loaded).
	CFG_VAR_DEF_BOOL(cfg_exit_after_level_loaded, "editor/exit_after_level_loaded", false);
	// The Play report of a module reload (.cache/hot_reload/last_play_snapshot.json) holds every world's whole state too.
	CFG_VAR_DEF_BOOL(cfg_hot_reload_dump_snapshot, "hot_reload/dump_snapshot", false);
	// Read like the window service does: "null" is a run without a display (--headless sets it).
	CFG_VAR_DEF_STRING(cfg_window_system, "window/system", "glfw");
	CFG_VAR_DEF_STRING(cfg_build_configuration, "build/configuration", "debuggame");


}

edt::editor_system::editor_system(desc::desc_system& desc_system_, res::resource_system& res_sys, rnd::render_system& rnd_sys, gui::gui_system& gui_sys)
	: m_scene_editor(m_state, m_services)
	, m_world_ctrl(m_state, m_services)
	, m_level_ctrl(m_state, m_services, m_scene_editor, m_world_ctrl)
	, desc_system(desc_system_)
	, m_res(res_sys)
	, m_rnd(rnd_sys)
	, m_gui(gui_sys)
{
}

edt::editor_system::~editor_system()
{
	if (m_agent) m_agent->shutdown();
	if (m_import) m_import->shutdown();
	// The layer may outlive this system: no game layout drawn through it any more.
	if (editor_layer) {
		editor_layer->set_game_layout(nullptr, nullptr, nullptr, nullptr);
		editor_layer->set_loading_screen(nullptr, nullptr);
		editor_layer->set_modal_probe(nullptr);
		editor_layer->set_frame_hook(nullptr);
	}
	if (m_play_component) {
		// Callbacks during Stop see no session, as with the owning pointer before; the abandoned one stays reachable.
		m_play = nullptr;
		m_modes = nullptr;
		m_play_component->shutdown(m_context_alive.lock() != nullptr);
		m_play = m_play_component->play();
	}
	// The layer may outlive this system; its menu bar must not reach live mode any more.
	if (editor_layer) editor_layer->set_menu_bar_status(nullptr);
	// The context (and its document_manager) outlives this system, which owns the scene document: let go of it first.
	// Gone already (storage torn down without editor_term): its destructor stopped the jobs and let go of the document.
	if (const std::shared_ptr<edt::editor_context> context = m_context_alive.lock()) {
		// Jobs run the importer and exporter of this system: none may be left running when they are destroyed.
		context->jobs().cancel_all_and_wait();
		if (m_asset_open) m_asset_open->data_documents().shutdown();
		context->documents().shutdown();
	}
	// Its "prefab" listener of document_closed reaches the problems component, which is declared before it and goes after it.
	if (m_prefab) {
		m_prefab->detach_document();
		m_prefab.reset();
	}
	if (auto ctx_mgr = m_services.ctx_manager.lock()) {
		ctx_mgr->set_on_game_layer_change(nullptr);
		ctx_mgr->set_on_reload_result(nullptr);
		ctx_mgr->set_on_level_switched(nullptr);
	}
}

void edt::editor_system::init(ds::app_data_storage& data)
{
	m_rnd.get_texture_manager().generate(res::tag(res::tag::memory, "__black"), {1,1}, rnd::driver::texture_header::TYPE::RGB8, {0, 0, 0});
	m_rnd.get_texture_manager().generate(res::tag(res::tag::memory, "__red"),   {1,1}, rnd::driver::texture_header::TYPE::RGB8, {255, 0, 0});

	m_res.warmup<res::text_resource>(res::tag("edt://templates/level.desc"));
	m_res.warmup<res::text_resource>(res::tag("edt://templates/world.desc"));

	editor_layer = std::make_shared<edt::editor_layer>();
	m_gui.push_layer(editor_layer);

	// Resolve services from data storage -> editor_services (the controllers were built on it already)
	m_services.ctx_manager = data.require_shared<scn::context_manager>();
	m_tasks = &data.require<ds::task_system>();
	// The editor puts its cameras back itself after a restore; a snapshot leaves them out. The base outlives an editor,
	// so a second editor on the same services finds them registered.
	if (auto& catalog = m_services.ctx_manager.lock()->catalog(); !catalog.find("edt.editor_camera")) {
		catalog.add_editor_only<edt::editor_camera_tag>("edt.editor_camera");
		catalog.add_editor_only<edt::prefab_camera_tag>("edt.prefab_camera");
	}
	// The editor scene is authored, not played: only run_in_edit systems tick (transforms, camera, animation preview).
	m_services.ctx_manager.lock()->level_set().set_mode(ecs::run_mode::edit);
	m_services.base_descs     = &desc_system;
	m_services.base_resources = &m_res;
	m_services.base_sfactory  = &data.require<ecs::system_factory>();
	m_services.base_assembler = &data.require<scn::ecs_assembler>();
	ASSERT_MSG(!data.has_value<edt::editor_context>(), "editor_system::init called twice: editor_context is already registered");
	// Filled in below, once the agent policy exists; job.*, dialog.list and problems.list ask it on every call (#935).
	const cmd::log_read_filter* agent_filter = data.has_value<cmd::log_read_filter>() ? &data.require<cmd::log_read_filter>() : nullptr;
	m_context = &data.construct<edt::editor_context>(m_res, m_rnd, m_gui, m_services,
		data.require<ds::task_system>(), data.require<cmd::command_registry>(), m_state, agent_filter);
	m_context->adopt(register_state_commands(m_context->actions().commands(), state_command_sources{
		m_context->documents(), m_context->selection(), m_services, m_context->asset_kinds(),
		[&res = m_res] { return res.get_resource_layers(); } }));
	m_context_alive = data.require_shared<edt::editor_context>();
	editor_layer->set_context(*m_context);
	m_import = std::make_unique<import_component>(import_component::ports{ .context = *m_context, .resources = m_res, .desc = desc_system,
		.tasks = data.require<ds::task_system>(), .layer = *editor_layer,
		.play = [this]() -> const play_session* { return m_play; },
		.exported = [this](const res::tag& tag) {
			if (m_state.editor_tag.is_valid()) m_asset_open->add_exported_to_scene(tag);
		} });
	m_context->dialogs().set_registrar([this](std::string_view id, std::function<bool()> draw) {
		editor_layer->register_implicit(id, std::move(draw));
	});
	const bool headless = cfg_edt_headless || *cfg_window_system == "null";
	if (headless) {
		data.require<cmd::command_registry>().set_headless(true);
		// Nobody answers a dialog: it is dismissed at once instead of waiting forever (dialog.answer is not for agents).
		dialog_auto_answer nobody;
		nobody.cancel_all = true;
		nobody.warn_headless = true;
		m_context->dialogs().set_auto_answer(nobody);
	}
	// #921: main's trust_library of the project's modules finds it in the storage; build.* and live mode ask it.
	if (!data.has_value<project_trust_service>()) {
		const std::optional<std::string> trust_key = data.has_value<prj::project_context>()
			? canonical_project_settings_key(data.require<prj::project_context>().root) : std::nullopt;
		data.construct<project_trust_service>(m_res, trust_key, static_cast<bool>(cfg_edt_trust_project));
	}
	m_trust = data.require_shared<project_trust_service>();
	m_registrations.push_back(m_context->actions().add({ .id = "project.trust", .label = "Trust Project Modules...",
		.menu_path = "Project", .disabled_tooltip = "Open a project first",
		.execute = no_args([this] { m_live->ask_trust(nullptr); }),
		.expose_as_tool = false,
		.enabled = [this] {
			const auto trust = m_trust.lock();
			return trust && trust->has_project();
		},
		// Only the person grants trust: an agent never sees or answers this question.
		.category = cmd::agent_category::not_for_agents }));
	// The panels own input routing: whichever scene image is hovered feeds its context, nothing else does.
	m_input_router = &data.require<inp::input_router>();
	m_input_router->set_default_target(std::nullopt);

	// What the Problems panel shows (#1004): before the panels, the prefab editor and the loads that report into it.
	m_problems = std::make_unique<problems_component>(problems_component::ports{ .context = *m_context, .state = m_state,
		.services = m_services, .worlds = m_world_ctrl,
		.assets = [this] { return m_asset_component.get(); },
		.open_asset = [this](const res::tag& tag, bool quiet) { return m_asset_open->open(tag, quiet); },
		.play_active = [this] { return m_play && m_play->active(); } });

	const auto asset_catalog = [this]() -> const picker::catalog* { return m_asset_component ? &m_asset_component->catalog() : nullptr; };
	// Create panels
	m_panels = std::make_unique<panels_component>(panels_component::ports{ .context = *m_context, .services = m_services, .state = m_state,
		.scene = m_scene_editor, .worlds = m_world_ctrl, .resources = m_res, .rnd = m_rnd, .gui = m_gui, .input_router = m_input_router,
		.problems = *m_problems,
		.capture = data.has_value<rnd::capture_service>() ? &data.require<rnd::capture_service>() : nullptr,
		// Load order, absolute paths and timings in the log text differ between runs and would show in screenshots.
		.capture_log = !(data.has_value<app::deterministic_time>() && data.require<app::deterministic_time>().enabled()),
		.asset_catalog = asset_catalog,
		.project_root = [&data]() -> std::optional<std::filesystem::path> {
			if (!data.has_value<prj::project_context>()) return std::nullopt;
			return data.require<prj::project_context>().root;
		},
		.input_actions = [&data]() -> gs::input_actions* {
			return data.has_value<gs::input_actions>() ? &data.require<gs::input_actions>() : nullptr;
		},
		.default_configuration = *cfg_build_configuration,
		.manual_build_running = [this] { return m_build && m_build->builder().is_manual_build_running(); },
		.stop_live_build = [this] { stop_live_build(); },
		.play_active = [this] { return m_play && m_play->active(); } });
	for (auto& renderer : register_desc_component_renderers(m_context->inspectors())) m_registrations.push_back(std::move(renderer));

	// Wire all controller + panel callbacks
	wire_controller_callbacks();
	// During Play the selected entity is the one of the play twin, which the views show.
	m_context->selection().set_registry_provider([this](const std::string& world) -> entt::registry* {
		scn::context_set* set = view_set(m_services);
		scn::world_context* found = set ? set->find(world) : nullptr;
		return found ? &found->state() : nullptr;
	});
	m_selection_connections = wire_selection(
		m_context->events(), m_context->selection(), m_state, m_scene_editor, m_world_ctrl,
		*m_panels->hierarchy(), *m_panels->inspector(),
		[this](ecs::entity ent) { m_panels->viewport()->set_selected_entity(ent); });
	m_panels->viewport()->set_selected_resolver([this] { return m_context->selection().entity(); });

	// Prefab editor (EPIC-25, #1004): after wire_selection, whose Inspector hand-over its selection listener follows.
	m_prefab = std::make_unique<prefab_editor_component>(prefab_editor_component::ports{
		*m_context, m_services, m_state, m_rnd, m_gui, m_input_router, *m_panels->inspector(), m_problems->desc() });

	m_panels->inspector()->set_open_prefab([this](const res::tag& tag) { m_prefab->open(tag); });

	asset_open_component::ports open_ports{ .context = *m_context, .services = m_services, .layer = *editor_layer, .resources = m_res,
		.state = m_state, .scene = m_scene_editor, .level_ctrl = m_level_ctrl, .desc_problems = m_problems->desc(),
		.browser = *m_panels->asset_browser(),
		.open_prefab = [this](const res::tag& tag) { m_prefab->open(tag); },
		.show_prefab = [this] { m_prefab->show(); },
		.play_active = [this] { return m_play && m_play->active(); },
		.refused_during_play = [this](std::string text) { return refused_during_play(m_play, m_context->dialogs(), std::move(text)); },
		.write_blocker = [this] { return project_write_blocker(); },
		.asset_index_wait = [this] { return asset_index_wait(m_asset_component ? m_asset_component->index() : nullptr); },
		.asset_catalog = asset_catalog };
	m_asset_open = std::make_unique<asset_open_component>(std::move(open_ports));
	m_asset_open->install(m_registrations);

	// Registration order is the render order and the View menu order (panels_component::install).
	m_panels->install(m_registrations, [this] { m_prefab->install(m_registrations); });

	auto& wnd_sys = data.require<wnd::window_system>();
	m_exit_action = [&wnd_sys] {
		if (auto win = wnd_sys.get_active_window())
			win->shutdown();
	};
	if (cfg_exit_after_level_loaded) {
		m_stop_after_level_loaded = [&data] { data.require<app::app_loop_service_interface>().request_stop(); };
	}

	m_state.set_save_handlers(
		[this] { return m_level_ctrl.quick_save_level(); },
		[this](const res::tag& target) { return m_level_ctrl.save_level_as(std::string(target.relative())); });

	level_load_component::ports level_ports{ .context = *m_context, .layer = *editor_layer, .level_ctrl = m_level_ctrl, .services = m_services,
		.resources = m_res, .state = m_state, .desc_problems = m_problems->desc(), .tasks = m_tasks,
		.stop_play = [this] { if (m_play && m_play->active()) m_play->stop(); },
		.refresh_world_problems = [this] { m_problems->refresh_world(); },
		.stop_after_level_loaded = m_stop_after_level_loaded };
	m_level_load = std::make_unique<level_load_component>(std::move(level_ports));
	m_game_layer = std::make_unique<game_layer_component>(game_layer_component::ports{ .services = m_services, .level_ctrl = m_level_ctrl,
		.level_load = *m_level_load, .prefab = *m_prefab, .asset_open = *m_asset_open,
		.stop_play = [this] { if (m_play && m_play->active()) m_play->stop(play_reason::user); } });

	m_file_menu = std::make_unique<file_menu_component>(file_menu_component::ports{ .context = *m_context, .resources = m_res, .state = m_state,
		.level_ctrl = m_level_ctrl, .level_load = *m_level_load, .prefab = *m_prefab, .asset_open = *m_asset_open, .problems = *m_problems,
		.import = *m_import,
		.project_manifest = data.has_value<prj::project_context>()
			? std::optional<std::filesystem::path>{ data.require<prj::project_context>().manifest_file } : std::nullopt,
		.projects_path = data.has_value<prj::project_context>() ? data.require<prj::project_context>().root.parent_path()
			: std::filesystem::current_path(),
		.exit = m_exit_action,
		.stop_play = [this] { if (m_play && m_play->active()) m_play->stop(); },
		.migration_blocker = [this] { return migration_blocker(); },
		.migration_stopped_reason = [this]() -> std::optional<std::string> { return m_migration ? m_migration->stopped_reason() : std::nullopt; },
		.startup_pending = [this] { return m_modes && m_modes->startup_pending(); },
		.assets = [this] { return m_asset_component.get(); } });
	m_file_menu->install(m_registrations);
	m_level_load->install(m_registrations);
	m_file_menu->install_edit_actions(m_registrations);
	// A5 (#291): the agent's edits of the scene. Offered over MCP by the agent policy (scene_edit: allowed by default).
	// #968: until the asset index is complete, an agent's guid is answered `busy` rather than judged against it.
	for (auto& command : register_scene_commands(m_context->actions().commands(),
			{ m_context->documents(), m_context->selection(), m_services, [this] { return person_is_editing(); },
				[this] { return asset_index_wait(m_asset_component ? m_asset_component->index() : nullptr); } })) {
		m_registrations.push_back(std::move(command));
	}
	// A7 (#451): the agent's edits of code/ through code://; without a code folder they answer no_project.
	for (auto& command : register_code_commands(m_context->actions().commands(), { m_res, &m_context->events() })) {
		m_registrations.push_back(std::move(command));
	}
	// #550: the agent's reads and writes of the desc files of res/, checked against the schemas first.
	{
		edt::desc_file_deps desc_files{ m_res, desc_system };
		desc_file_layout layout = m_asset_open->desc_layout();
		desc_files.top_layer = std::move(layout.top_layer);
		desc_files.layers = std::move(layout.layers);
		desc_files.open_level_files = std::move(layout.open_level_files);
		desc_files.write_blocker = std::move(layout.write_blocker);
		desc_files.asset_index_wait = std::move(layout.asset_index_wait);
		desc_files.events = &m_context->events();
		for (auto& command : register_desc_file_commands(m_context->actions().commands(), std::move(desc_files))) {
			m_registrations.push_back(std::move(command));
		}
	}
	// EPIC-58 (#759, #761): the migration commands, the Project Migration window and the offer to roll an unfinished migration
	// back. Refused while Play, a load, a build or a module reload, an import, the export dialog or a git write runs (busy), and
	// with an uncommitted edit or any dirty document (needs_input).
	m_migration = std::make_unique<migration_component>(migration_component::ports{
		.context = *m_context, .services = m_services, .resources = m_res, .layer = *editor_layer, .level_ctrl = m_level_ctrl,
		.state = m_state, .prefab = *m_prefab, .asset_open = *m_asset_open, .level_load = *m_level_load,
		.project = data.has_value<prj::project_context>() ? &data.require<prj::project_context>() : nullptr,
		.project_held = data.has_value<prj::held_project>(),
		.headless = headless,
		.log_filter = data.has_value<cmd::log_read_filter>() ? &data.require<cmd::log_read_filter>() : nullptr,
		.busy_reason = [this]() -> std::optional<std::string> {
			if (m_play && m_play->active()) return std::string{ "Play is running: stop it first" };
			if (m_modes && m_modes->startup_pending()) return std::string{ "the start level is loading" };
			if (auto loading = load_in_progress()) return loading;
			if (m_build && m_build->builder().is_building()) return std::string{ "a game build is running" };
			if (editor_layer->has_implicit_prefix(game_reloader::k_reload_frame_id) || editor_layer->has_implicit_prefix(live_mode::k_reload_frame_id)) {
				return std::string{ "a reload of the game module is waiting" };
			}
			if (m_import->agent_import_running()) return std::string{ "a model import asked for by an agent is running" };
			if (m_import->dialog_open()) return std::string{ "the asset export dialog is open" };
			if (m_vcs && m_vcs->writes_running()) return std::string{ "a git operation (pull, checkout, commit...) is running" };
			if (m_import->person_import_running()) return std::string{ "a model import is running" };
			return std::nullopt;
		},
		.unsaved_reason = [this]() -> std::optional<std::string> {
			if (person_is_editing()) return std::string{ "a person is editing a value (an inspector field or a gizmo drag): commit it first" };
			if (m_state.is_dirty()) return std::string{ "the level has unsaved changes: save or discard them first" };
			std::optional<std::string> dirty;
			m_context->documents().for_each([&dirty](document& open) {
				if (!dirty && open.is_dirty()) dirty = std::format("'{}' has unsaved changes: save or discard them first", open.title());
			});
			return dirty;
		},
		.game_layer_generation = [this] { return m_game_layer ? m_game_layer->generation() : std::uint64_t{ 0 }; },
		.approved_internally = [this](std::string_view ticket) {
			return m_agent != nullptr && m_agent->access() != nullptr && m_agent->access()->confirmations().approved_internally(ticket);
		},
		.rebuild_assets = [this] {
			if (m_asset_component) m_asset_component->rebuild();
		},
		.startup_pending = [this] { return m_modes && m_modes->startup_pending(); } });
	m_migration->install(m_registrations, [this](const std::shared_ptr<panel_base>& panel) { m_panels->add(m_registrations, panel, dock_slot::floating, false); });
	m_file_menu->attach_window_close(wnd_sys);

	game_build_component::ports build_ports{ .context = *m_context, .layer = *editor_layer, .settings_panel = *m_panels->build_settings(),
		.swap_gate = m_game_layer->swap_gate(), .trust = m_trust,
		.project = data.has_value<prj::project_context>() ? &data.require<prj::project_context>() : nullptr,
		.default_configuration = *cfg_build_configuration,
		.module_host = [&data]() -> core::project_module_host* {
			return data.has_value<core::project_module_host>() ? &data.require<core::project_module_host>() : nullptr;
		},
		.ask_trust = [this](std::function<void()> then) { m_live->ask_trust(std::move(then)); },
		.migration_blocker = [this] { return migration_blocker(); },
		.load_in_progress = [this] { return load_in_progress(); },
		.level_switch_pending = [this] {
			const auto contexts = m_services.ctx_manager.lock();
			return contexts && contexts->level_switch_pending();
		},
		.prefab_has_unapplied_edits = [this] { return m_prefab->dirty(); },
		.live_mode_enabled = [this] { return m_live && m_live->enabled(); },
		.play = [this] { return m_play; } };
	m_build = std::make_unique<game_build_component>(std::move(build_ports));

	m_live = std::make_unique<live_mode_component>(live_mode_component::ports{
		.context = *m_context, .resources = m_res, .layer = *editor_layer, .trust = m_trust, .builder = m_build->builder(),
		.reloader = m_build->reloader(), .build_settings = *m_panels->build_settings(), .problems = *m_panels->problems(),
		.project = data.has_value<prj::project_context>() ? &data.require<prj::project_context>() : nullptr,
		.module_host = [&data]() -> core::project_module_host* {
			return data.has_value<core::project_module_host>() ? &data.require<core::project_module_host>() : nullptr;
		},
		.migration_blocker = [this] { return migration_blocker(); },
		.load_in_progress = [this] { return load_in_progress(); },
		.play_active = [this] { return m_play && m_play->active(); },
		.tools_changed = [this] { if (m_agent) m_agent->notify_tools_changed(); } });
	m_live->install();
	create_play_session(data);

	m_build->install(m_registrations);

	vcs_component::ports vcs_ports{ *m_context, *m_panels->git() };
	vcs_ports.project_root = [&data]() -> std::optional<std::filesystem::path> {
		if (!data.has_value<prj::project_context>()) return std::nullopt;
		return data.require<prj::project_context>().root;
	};
	vcs_ports.live_mode_on = [this] { return m_live && m_live->enabled(); };
	m_vcs = std::make_unique<vcs_component>(std::move(vcs_ports));
	if (data.has_value<prj::project_context>()) {
		const prj::project_context& project = data.require<prj::project_context>();
		asset_component::ports asset_ports{ *m_context, m_res, *m_panels->asset_browser(), project };
		asset_ports.vcs_busy = [this] { return m_vcs && m_vcs->writes_running(); };
		asset_ports.git_dir = [this]() -> std::optional<std::filesystem::path> {
			return m_vcs ? m_vcs->git_dir() : std::nullopt;
		};
		asset_ports.descs = [this]() -> const desc::desc_system* { return m_services.descs(); };
		asset_ports.game_layer_generation = [this] { return m_game_layer ? m_game_layer->generation() : std::uint64_t{ 0 }; };
		m_asset_component = std::make_unique<asset_component>(std::move(asset_ports));
		m_asset_component->install(m_registrations);
	}
	m_vcs->install(m_registrations);

	m_startup = std::make_unique<startup_component>(startup_component::ports{ .context = *m_context, .layer = *editor_layer,
		.level_load = *m_level_load,
		.held = data.has_value<prj::held_project>() ? &data.require<prj::held_project>() : nullptr,
		.trust = m_trust, .headless = headless,
		.start_level_name = [&data](const std::string& configured) { return scn::start_level_name(data, configured); },
		.modes = [this] { return m_modes; },
		.ask_trust = [this](std::function<void()> on_trusted) { m_live->ask_trust(std::move(on_trusted)); },
		.exit = m_exit_action, .stop_after_level_loaded = m_stop_after_level_loaded });
	m_startup->install_start_level();

	m_migration->install_startup();

	m_startup->install_trust_check();

	m_migration->install_open_on_start();

	{
		agent_component::ports agent_ports{ .context = *m_context, .resources = m_res, .commands = data.require<cmd::command_registry>(),
			.tasks = data.require<ds::task_system>(), .layer = *editor_layer, .vcs = *m_vcs, .headless = headless };
		if (data.has_value<cmd::log_read_filter>()) agent_ports.log_filter = &data.require<cmd::log_read_filter>();
		if (data.has_value<prj::project_context>()) agent_ports.project_root = data.require<prj::project_context>().root;
		if (data.has_value<platform::protocol_stdout>()) {
			agent_ports.stdio_write = [fd = data.require<platform::protocol_stdout>().fd](std::string_view line) {
				return platform::write_all(fd, line);
			};
		}
		agent_ports.live_enabled = [this] { return m_live != nullptr && m_live->enabled(); };
		agent_ports.any_unsaved = [this] {
			return any_unsaved([this](const std::function<void(document&)>& visit) { m_context->documents().for_each(visit); });
		};
		agent_ports.discard_changes = [this] {
			m_state.mark_clean();
			m_prefab->drop_dirty_document();
		};
		agent_ports.request_stop = [&data] { data.require<app::app_loop_service_interface>().request_stop(); };
		// The guard is in place before the server can take a call.
		m_vcs->guard_config_files(editor_config_files());
		m_agent = std::make_unique<agent_component>(std::move(agent_ports));
		m_agent->install(m_registrations);
		m_agent_ui = std::make_unique<agent_ui_component>(agent_ui_component::ports{ .context = *m_context, .layer = *editor_layer,
			.commands = data.require<cmd::command_registry>(), .agent = *m_agent, .headless = headless });
		m_agent_ui->install(m_registrations);
	}
}

edt::agent_server* edt::editor_system::agent_server_object()
{
	return m_agent ? m_agent->server() : nullptr;
}

edt::agent_access* edt::editor_system::agent_access_object()
{
	return m_agent ? m_agent->access() : nullptr;
}

void edt::editor_system::tick_live_mode()
{
	if (m_live) m_live->tick();
}

void edt::editor_system::tick_repository_watcher()
{
	if (m_vcs && m_context) m_vcs->tick();
}

edt::asset_database* edt::editor_system::assets()
{
	return m_asset_component ? &m_asset_component->database() : nullptr;
}

void edt::editor_system::tick_asset_problems()
{
	if (m_asset_component && m_context) {
		m_asset_component->tick();
	}
}

void edt::editor_system::stop_live_build()
{
	// Not cancel(): that drops the person's request waiting for the live build and gives live mode no outcome.
	if (m_build) m_build->builder().stop_background();
}

void edt::editor_system::create_play_session(ds::app_data_storage& data)
{
	m_play_component = std::make_unique<play_component>(play_component::ports{
		.context = *m_context, .services = m_services, .level = m_state, .worlds = m_world_ctrl, .resources = m_res,
		.input_router = m_input_router, .layer = *editor_layer, .swap_gate = m_game_layer->swap_gate(), .hierarchy = *m_panels->hierarchy(),
		.inspector = *m_panels->inspector(), .viewport = *m_panels->viewport(), .game_view = *m_panels->game_view(),
		.problems = *m_panels->problems(), .preflight = m_problems->play_preflight(),
		.project = data.has_value<prj::project_context>() ? &data.require<prj::project_context>() : nullptr,
		.module_host = [&data]() -> core::project_module_host* {
			return data.has_value<core::project_module_host>() ? &data.require<core::project_module_host>() : nullptr;
		},
		.frame_commands = data.has_value<app::frame_command_registrations>() && data.has_value<app::frame_control>()
			? &data.require<app::frame_command_registrations>() : nullptr,
		.frame_control = data.has_value<app::frame_command_registrations>() && data.has_value<app::frame_control>()
			? &data.require<app::frame_control>() : nullptr,
		.report_full_snapshot = cfg_hot_reload_dump_snapshot,
		.start_blocker = [this] {
			if (auto migrating = migration_blocker()) return migrating;
			return load_in_progress();
		},
		.person_is_editing = [this] { return person_is_editing(); },
		.agent_mode_of = [this](std::string_view command_id) {
			// No agent server: no agent can call at all.
			if (!agent_access_object()) return cmd::policy_mode::allow;
			const cmd::command_desc* command = m_context->actions().find(command_id);
			return command ? agent_access_object()->policy().effective(*command).mode : cmd::policy_mode::deny;
		} });
	m_play_component->install(m_registrations);
	m_play = m_play_component->play();
	m_modes = m_play_component->modes();
	// The start level of --game (#518). Also off once its load's implicit callback is gone, so the screen never outlives it.
	editor_layer->set_loading_screen(
		[this] { return m_modes && m_modes->startup_pending() && editor_layer->has_implicit_prefix("edt/load_level"); },
		[this] { draw_loading_screen(loading_screen_text(m_modes->startup_level_label())); });
	// The Input Actions panel waiting for a key is a modal too: Ctrl+S and the rest must not fire under the player's key (#470).
	editor_layer->set_modal_probe([this] {
		return m_context->dialogs().pending() > 0 || (m_panels->input_actions() && m_panels->input_actions()->capturing());
	});
	// Bindings two actions share are Problems (source "input"), whether or not the panel is open.
	editor_layer->set_frame_hook([this, &data] {
		// Before the shortcuts of the frame: Ctrl+Z goes to the document of the window the person is in.
		if (m_asset_open) m_asset_open->data_documents().follow_focus();
		if (!m_context || !data.has_value<gs::input_actions>()) return;
		const auto& actions = data.require<gs::input_actions>();
		m_problems->follow_input_actions(actions);
	});
	m_level_ctrl.on_worlds_replaced = [this] { m_problems->refresh_world(); };
	m_problems->install();
	// Popups and dialog_service modals are seen by ImGui; the agent's question is an ordinary window.
	m_panels->game_view()->set_input_blocked([this] {
		return m_context->dialogs().pending() > 0 || (agent_access_object() && !agent_access_object()->confirmations().pending().empty());
	});

	editor_layer->set_menu_bar_status([this] {
		// Left of the others, which place themselves at the right edge. Nothing at all unless an agent can connect or a policy waits.
		if (m_agent_ui) m_agent_ui->draw_status();
		m_play_component->draw_status();
		m_file_menu->draw_status();
		if (const auto trust = m_trust.lock()) {
			if (const auto held = held_modules_status(trust->held_modules())) {
				ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f), "%s", held->c_str());
			}
		}
		m_live->draw_status();
	});
}

void edt::editor_system::wire_controller_callbacks()
{
	// on_before_modify and on_world_switched go through the selection service (edt_selection_wiring.cpp).
	const auto save_camera = [this](entt::registry& reg) { save_camera_state(reg, m_state.camera); };
	const auto inject_editor_camera = [this](entt::registry& reg) { inject_camera(reg, m_state.camera); };
	m_scene_editor.on_save_camera = save_camera;
	m_scene_editor.on_inject_camera = inject_editor_camera;

	m_world_ctrl.on_save_camera = save_camera;
	m_world_ctrl.on_inject_camera = inject_editor_camera;

	m_level_ctrl.on_editor_world_reloaded = [] {
		egLOG("Editor", "Editor world reloaded");
	};
	if (auto ctx_mgr = m_services.ctx_manager.lock()) {
		ctx_mgr->set_on_reload([this] { m_level_ctrl.resync_worlds_after_reload(); });
		// Also a reload that failed: its errors stay in Problems until a clean one replaces them.
		ctx_mgr->set_on_reload_result([this](const res::tag&, const res::res_handle<scn::level_desc>& handle) {
			m_problems->desc().report(m_context->problems(), "level", diagnostics_of(handle), m_state.editor_tag, m_state.level_tag,
				level_locator(handle, m_state.editor_tag, m_state.level_tag));
		});
		// The set "level" is the edited document: a game's request_level only ever replaces the worlds of Play (#564).
		ctx_mgr->set_level_set_switchable(false);
		ctx_mgr->set_on_level_switched([this](scn::context_set& set) {
			if (set.name() == k_play_set && m_play) m_play->on_level_switched();
		});
	}
}

// ─── Level load ─────────────────────────────────────────────────────────────

void edt::editor_system::run_implicit_callbacks()
{
	if (editor_layer) editor_layer->process_implicit();
}

bool edt::editor_system::begin_level_load(const res::tag& original_tag)
{
	return m_level_load->begin(original_tag);
}

bool edt::editor_system::level_load_in_progress() const
{
	return m_level_load && m_level_load->in_progress();
}

void edt::editor_system::tick_level_settle()
{
	if (m_level_load) m_level_load->tick_settle();
}

void edt::editor_system::go_to_problem(const std::string& source, const problem& item)
{
	m_problems->go_to_problem(source, item);
}

std::optional<std::string> edt::editor_system::migration_blocker() const
{
	return m_migration ? m_migration->blocker() : std::nullopt;
}

std::optional<std::string> edt::editor_system::project_write_blocker()
{
	return m_migration ? m_migration->project_write_blocker() : std::nullopt;
}

std::optional<std::string> edt::editor_system::load_in_progress() const
{
	if (m_level_load && m_level_load->in_progress()) return std::string{ "a level is loading" };
	if (m_asset_open && m_asset_open->adding_to_scene()) return std::string{ "an asset is being added to the scene" };
	return std::nullopt;
}

bool edt::editor_system::person_is_editing() const
{
	// A text field that only has the keyboard is no gesture; a typed value not yet committed is the inspector's pending commit.
	return (ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput) || (m_panels && m_panels->inspector() && m_panels->inspector()->has_pending_commit())
		|| (m_panels && m_panels->viewport() && m_panels->viewport()->is_dragging_gizmo());
}

