#include "edt_editor_system.h"
#include "edt_asset_database.h"
#include "edt_asset_index.h"
#include "edt_asset_picker.h"
#include "edt_document_creator.h"
#include "edt_asset_problems.h"
#include "edt_begin_then_poll.h"
#include <algorithm>
#include <chrono>
#include <snk/editor/edt_editor_context.h>
#include <snk/common/engine_assert.h>
#include <snk/common/fs_paths.h>
#include "edt_selection_wiring.h"
#include "edt_prefab_document.h"
#include "edt_editor_camera.h"
#include <snk/editor/edt_document_manager.h>

#include "edt_editor_layer.h"
#include "edt_scene_hierarchy_panel.h"
#include "edt_inspector_panel.h"
#include "edt_override_edit.h"
#include "edt_viewport_panel.h"
#include "edt_game_view_panel.h"
#include "edt_console_panel.h"
#include "edt_problems_panel.h"
#include "edt_blackboard_panel.h"
#include "edt_input_actions_panel.h"
#include "edt_asset_browser_panel.h"
#include "edt_code_section.h"
#include <snk/platform/plt_shell_open.h>
#include "edt_prefab_editor_panel.h"
#include "edt_data_document_host.h"
#include "edt_component_renderers.h"
#include "edt_builtin_asset_kinds.h"
#include "edt_model_importer.h"
#include "edt_process_decoder.h"
#include "edt_asset_exporter.h"
#include "edt_asset_export_dialog.h"
#include "edt_async_import_task.h"
#include "edt_input_target.h"
#include "edt_edit_menu.h"
#include "edt_file_menu.h"
#include "edt_template_ids.h"
#include "edt_agent_model_import.h"
#include "edt_scene_commands.h"
#include "edt_agent_sessions.h"
#include "edt_code_commands.h"
#include "edt_desc_file_commands.h"
#include "edt_migration_commands.h"
#include "edt_migration_window_model.h"
#include "edt_migration_panel.h"
#include "edt_migration_startup.h"
#include "edt_game_builder.h"
#include "edt_game_reloader.h"
#include "edt_build_jobs.h"
#include "edt_build_settings_model.h"
#include "edt_build_settings_panel.h"
#include "edt_git_panel.h"
#include "edt_world_systems_panel.h"
#include "edt_vcs_service.h"
#include "edt_forge_account_model.h"
#include "edt_forge_account_panel.h"
#include "edt_forge_service.h"
#include "edt_github_provider.h"
#include "edt_curl_transport.h"
#include "edt_repository_watcher.h"
#include "edt_live_mode.h"
#include "edt_play_commands.h"
#include "edt_runtime_commands.h"
#include "edt_bus_commands.h"
#include "edt_play_session.h"
#include "edt_editor_mode.h"
#include "edt_editor_mode_commands.h"
#include <snk/core/core_project_modules.h>
#include "edt_main_menu.h"
#include "edt_agent_access.h"
#include "edt_agent_model_guard.h"
#include "edt_agent_server.h"
#include "edt_agent_server_settings.h"
#include "edt_agent_policy_model.h"
#include "edt_agents_panel.h"
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
#include <snk/platform/plt_process.h>
#include <snk/platform/plt_secret.h>
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

// Editor/data (editor_module): the project templates live under it.
CFG_VAR_EXT_PATH(cfg_edt_data_path);
// --headless (edt_editor_init.cpp).
CFG_VAR_EXT_BOOL(cfg_edt_headless);

namespace {
	template<class T>
	std::vector<res::diagnostic> diagnostics_of(const res::res_handle<T>& handle)
	{
		res::diagnostics_collector collector;
		handle.collect_into(collector);
		return collector.items();
	}

	// Where the diagnostics of a load of the level lead, by that load (`copy` is its working copy, `file` its own file).
	edt::desc_problem_publisher::locator level_locator(const res::res_handle<scn::level_desc>& handle, const res::tag& copy,
		const res::tag& file)
	{
		if (!handle.is_ready()) return {};
		struct located
		{
			// Kept with its worlds: their roots point into it.
			std::shared_ptr<scn::level_desc> desc;
			edt::level_worlds worlds;
		};
		auto held = std::make_shared<located>();
		held->desc = handle.get_sync();
		if (!held->desc) return {};
		held->worlds = edt::level_worlds_of(*held->desc, file.string(), { copy.string(), file.string() });
		return [held](const res::diagnostic& item) { return edt::locate_desc(held->worlds, item.resource, item.json_path); };
	}
}

namespace {

	// Play locks the document: what would add to the scene is refused where the person sees it, not only in the log.
	bool refused_during_play(const edt::play_session* play, edt::dialog_service& dialogs, std::string text)
	{
		if (!play || !play->active()) return false;
		egLOG_WARN("play", "{}", text);
		dialogs.message(edt::message_level::warning, std::move(text));
		return true;
	}

	std::string utf8_of(const std::filesystem::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

	// Switching projects restarts the editor (EPIC-42 #158): the same program and config file, the other project.
	// Overrides given with --set are not carried over.
	// --open-migration (#761): the new editor opens the Project Migration window once the project is open.
	std::optional<std::string> start_editor_on(const std::filesystem::path& manifest_file, bool allow_engine_version_mismatch,
		bool open_migration)
	{
		const std::filesystem::path program = platform::current_executable();
		if (program.empty()) {
			return std::string{ "the editor cannot find its own program file" };
		}
		std::vector<std::string> args;
		if (const auto config = cfg::loaded_config_path(); !config.empty()) {
			args.insert(args.end(), { "--config", utf8_of(config) });
		}
		args.insert(args.end(), { "--project", utf8_of(manifest_file) });
		if (allow_engine_version_mismatch) {
			args.push_back("--allow-engine-version-mismatch");
		}
		if (open_migration) {
			args.push_back("--open-migration");
		}
		egLOG_INFO("Editor/Project", "restarting the editor on '{}'", utf8_of(manifest_file));
		auto started = platform::spawn_detached(program, args);
		if (!started) {
			egLOG_ERROR("Editor/Project", "the editor was not restarted on '{}': {}", utf8_of(manifest_file), started.error);
			return started.error;
		}
		return std::nullopt;
	}

	// Level opened at startup, e.g. --set editor/startup_level levels/smoke_level.desc (automation, headless runs).
	CFG_VAR_DEF_STRING(cfg_startup_level, "editor/startup_level", "");
	// "game" (--game, #294): once the start level has loaded, the Game view fills the window and Play starts; "editor":
	// the panels. Anything else warns and is "editor".
	CFG_VAR_DEF_STRING(cfg_start_mode, "editor/start_mode", "editor");
	// --game=res://<level>: the level game mode starts on, instead of editor/startup_level. Only a res:// tag is taken.
	CFG_VAR_DEF_STRING(cfg_game_level, "editor/game_level", "");
	// Opens the Game view at startup (automation); otherwise it is toggled from the View menu.
	CFG_VAR_DEF_BOOL(cfg_show_game_view, "editor/show_game_view", false);
	// The run ends with 0 once the first loaded level settled (#669; CI: the headless run proves the level loaded).
	CFG_VAR_DEF_BOOL(cfg_exit_after_level_loaded, "editor/exit_after_level_loaded", false);
	// Logs the screen rectangle of the Asset Browser entries (Editor/UiProbe), so a headless smoke script finds an item by name.
	CFG_VAR_DEF_BOOL(cfg_ui_probe, "editor/ui_probe", false);
	// The MCP server for AI agents (read-only: log, jobs, dialogs, screenshots). Off unless the user turns it on.
	// "" (not set), true or false: an explicit false wins over the person's own switch (parse_cfg_switch).
	CFG_VAR_DEF_STRING(cfg_agent_server, "editor/agent_server", "");
	// MCP over stdin/stdout for one agent (--mcp-stdio, #452); main hands the saved stdout over as platform::protocol_stdout.
	CFG_VAR_DEF_BOOL(cfg_mcp_stdio, "editor/mcp_stdio", false);
	// --open-migration (#761): once the project is open and the start level settled, the Project Migration window opens
	// with a fresh plan ("Migrate..." of the engine version question restarts the editor with it). Ignored headless.
	CFG_VAR_DEF_BOOL(cfg_open_migration, "editor/open_migration", false);
	// The Play report of a module reload (.cache/hot_reload/last_play_snapshot.json) holds every world's whole state too.
	CFG_VAR_DEF_BOOL(cfg_hot_reload_dump_snapshot, "hot_reload/dump_snapshot", false);
	// Read like the window service does: "null" is a run without a display (--headless sets it).
	CFG_VAR_DEF_STRING(cfg_window_system, "window/system", "glfw");
	// 0: the operating system picks a free port; it is written to <project dir>/.snk/mcp.json with the token.
	CFG_VAR_DEF_INT(cfg_agent_server_port, "editor/agent_server_port", 0);
	// Minutes without a request after which an agent's MCP session is closed (its "allow until the end of the session" with it); 0: never.
	CFG_VAR_DEF_INT(cfg_agent_session_idle_minutes, "editor/agent_session_idle_minutes", 30);
	// The memory the decoder process may use for an agent's model (#647), in MiB; 0: 2048. Kept between 256 and 8192.
	CFG_VAR_DEF_INT(cfg_import_decoder_memory_mb, "import/decoder_memory_mb", 0);
	// Where .snk/mcp.json goes; empty: the open project's root, or the working directory without a project.
	CFG_VAR_DEF_STRING(cfg_project_dir, "editor/project_dir", "");
	// --agent-policy <file> (#452): a person's answers to the agent policy's `ask`, given in advance for a headless run
	// (agent_policy::set_preanswers_file). Relative to the working directory.
	CFG_VAR_DEF_PATH(cfg_agent_policy_file, "editor/agent_policy_file", "");
	// Build > Build Game (#434). Empty paths: found by the editor (see game_build_sdk_dir, edt_toolchain.h).
	CFG_VAR_DEF_PATH(cfg_build_sdk_dir, "build/sdk_dir", "");
	CFG_VAR_DEF_PATH(cfg_build_cmake, "build/cmake", "");
	CFG_VAR_DEF_PATH(cfg_build_vs_path, "build/vs_path", "");
	CFG_VAR_DEF_STRING(cfg_build_vs_min_version, "build/vs_min_version", "17.14");
	CFG_VAR_DEF_STRING(cfg_build_configuration, "build/configuration", "debuggame");
	// The git program of version control (#454); empty: git on PATH.
	CFG_VAR_DEF_PATH(cfg_vcs_git, "vcs/git", "");

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

	// A desc file that says it is something else than a level (`"__type": "material_desc"`, a prefab...). Reading it as a level
	// asserts in the desc adapter, so it is refused before. A file that is no JSON, or says nothing, fails the load as before.
	bool names_another_desc_type(const std::vector<std::byte>& bytes)
	{
		boost::json::error_code ec;
		boost::json::parse_options options;
		options.max_depth = 64;
		const boost::json::value parsed = boost::json::parse(
			std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), ec, {}, options);
		if (ec || !parsed.is_object()) return false;
		const boost::json::value* type = parsed.as_object().if_contains("__type");
		return type != nullptr && type->is_string() && std::string_view(type->as_string()) != scn::level_desc::__type;
	}

	// What the level file at a project path is in the layers of res:// (a lower content root, the engine), not only the top
	// one: an agent's save_as onto a file that only a lower layer has would shadow it, and its open of such a level works.
	// A link or a folder in any layer is unsafe; a layer that is not a folder (a RAM or archive resolver) is asked itself.
	edt::level_file_state inspect_level_in_layers(const res::resource_system& resources, const std::string& relative)
	{
		bool found = false;
		for (const std::filesystem::path& root : resources.get_resource_layers()) {
			switch (edt::inspect_level_file(root, relative)) {
			case edt::level_file_state::unsafe: return edt::level_file_state::unsafe;
			case edt::level_file_state::file: found = true; break;
			case edt::level_file_state::missing: break;
			}
		}
		if (found) return edt::level_file_state::file;
		if (const auto raw = resources.fetch_data(res::tag::make(relative))) {
			raw->wait();
			if (raw->is_ready()) return edt::level_file_state::file;
		}
		return edt::level_file_state::missing;
	}
}

edt::editor_system::editor_system(desc::desc_system& desc_system_, res::resource_system& res_sys, rnd::render_system& rnd_sys, gui::gui_system& gui_sys)
	: m_scene_editor(m_state, m_services)
	, m_world_ctrl(m_state, m_services)
	, m_level_ctrl(m_state, m_services, m_scene_editor, m_world_ctrl)
	, desc_system(desc_system_)
	, m_res(res_sys)
	, m_rnd(rnd_sys)
	, m_gui(gui_sys)
#if SNK_HAS_ASSIMP
	, m_model_importer(std::make_unique<edt::model_importer>(desc_system_, res_sys))
#endif
	, m_asset_exporter(std::make_unique<edt::asset_exporter>(res_sys))
{
}

edt::editor_system::~editor_system()
{
	// Its thread and the calls it queued use the registry and the task system: stop it before anything else goes.
	m_agent_server.reset();
	m_session_connections.clear();
	m_agent_access.reset();
	if (m_agent_argument_checks) m_agent_argument_checks->set_agent_argument_check(nullptr);
	// The layer may outlive this system: no game layout drawn through it any more.
	if (editor_layer) {
		editor_layer->set_game_layout(nullptr, nullptr, nullptr, nullptr);
		editor_layer->set_loading_screen(nullptr, nullptr);
		editor_layer->set_modal_probe(nullptr);
		editor_layer->set_frame_hook(nullptr);
	}
	m_mode_changed.disconnect();
	m_modes.reset();
	if (m_play) {
		// Stop brings the level back and reports through the context: only while the context is there.
		if (m_context_alive.lock()) {
			m_play.reset();
		} else {
			m_play->abandon();
		}
	}
	// The layer may outlive this system; its menu bar must not reach live mode any more.
	if (editor_layer) editor_layer->set_menu_bar_status(nullptr);
	// The context (and its document_manager) outlives this system, which owns the scene document: let go of it first.
	// Gone already (storage torn down without editor_term): its destructor stopped the jobs and let go of the document.
	if (const std::shared_ptr<edt::editor_context> context = m_context_alive.lock()) {
		// Jobs run the importer and exporter of this system: none may be left running when they are destroyed.
		context->jobs().cancel_all_and_wait();
		if (m_data_documents) m_data_documents->shutdown();
		context->documents().shutdown();
	}
	if (m_prefab_panel) {
		m_prefab_panel->set_context(nullptr);
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
	m_context = &data.construct<edt::editor_context>(m_res, m_rnd, m_gui, m_services,
		data.require<ds::task_system>(), data.require<cmd::command_registry>(), m_state);
	m_context->adopt(register_state_commands(m_context->actions().commands(), state_command_sources{
		m_context->documents(), m_context->selection(), m_services, m_context->asset_kinds(),
		[&res = m_res] { return res.get_resource_layers(); } }));
	m_context_alive = data.require_shared<edt::editor_context>();
	editor_layer->set_context(*m_context);
	m_export_dialog = std::make_unique<edt::asset_export_dialog>(*m_asset_exporter, m_context->jobs(), m_res);
	{
		edt::agent_import_options import_options;
#if SNK_HAS_ASSIMP
		// An agent's model is parsed in a process of its own, with a memory limit and killed at its deadline (#647). There is
		// no fallback to parsing it here: without the decoder the command says so.
		{
			std::string memory_warning;
			edt::decode_runner_config decoder_config;
			decoder_config.memory_limit_bytes = edt::decoder_memory_limit(static_cast<std::int64_t>(*cfg_import_decoder_memory_mb), memory_warning);
			if (!memory_warning.empty()) egLOG_WARN("Editor/Import", "{}", memory_warning);
			m_process_decoder = std::make_unique<edt::process_decoder>(m_res, std::move(decoder_config));
		}
		import_options.import = [decoder = m_process_decoder.get()](const edt::import_source& source, ds::task_context* task) {
			return decoder->import(source, task);
		};
		import_options.scan = [decoder = m_process_decoder.get()](const edt::import_source& source, ds::task_context* task) {
			return decoder->scan(source, task);
		};
		import_options.unavailable_reason = [decoder = m_process_decoder.get()] { return decoder->unavailable_reason(); };
#endif
		import_options.resource_layers = [this] { return m_res.get_resource_layers(); };
		import_options.resources_path = [this] { return m_res.get_resources_path(); };
		import_options.blocker = [this]() -> std::optional<std::string> {
			if (m_play && m_play->active()) return std::string{ "Stop Play to import." };
			if (m_export_dialog_active) return std::string{ "an import is waiting in the editor's export dialog" };
#if SNK_HAS_ASSIMP
			if (m_importing) return std::string{ "an import is already running in the editor" };
#endif
			return std::nullopt;
		};
		m_agent_import = std::make_unique<edt::agent_model_import>(*m_asset_exporter, m_context->jobs(), m_res, std::move(import_options));
	}
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
	// The panels own input routing: whichever scene image is hovered feeds its context, nothing else does.
	m_input_router = &data.require<inp::input_router>();
	m_input_router->set_default_target(std::nullopt);

	// Create panels
	// Load order, absolute paths and timings in the log text differ between runs and would show in screenshots.
	create_panels(!(data.has_value<app::deterministic_time>() && data.require<app::deterministic_time>().enabled()));
	for (auto& renderer : register_desc_component_renderers(m_context->inspectors())) m_registrations.push_back(std::move(renderer));
	m_inspector_panel->set_desc_source([this] { return m_services.descs(); });
	const auto asset_catalog = [this]() -> const picker::catalog* { return m_asset_catalog.get(); };
	m_inspector_panel->set_asset_source(asset_catalog);
	{
		// One composed view of the instances for the inspector and the hierarchy; edits and pushes invalidate it.
		auto view_cache = std::make_shared<override_edit::view_cache>(m_services, m_state);
		m_inspector_panel->set_view_cache(view_cache);
		m_hierarchy_panel->set_view_cache(std::move(view_cache));
	}
	// "Changed by agent" (AI_INTEGRATION.md §3.4): nodes of the active world an agent's edits changed, not saved or accepted.
	const auto changed_by_agent = [this](std::uint64_t node_id) {
		return m_state.has_active_world() && m_state.agent_changes().contains({ static_cast<std::size_t>(m_state.active_world_idx), node_id });
	};
	m_hierarchy_panel->set_agent_marks(changed_by_agent);
	m_inspector_panel->set_agent_marks(changed_by_agent);
	create_viewport_panel();

	// Wire all controller + panel callbacks
	wire_controller_callbacks();
	wire_panel_callbacks();
	// During Play the selected entity is the one of the play twin, which the views show.
	m_context->selection().set_registry_provider([this](const std::string& world) -> entt::registry* {
		scn::context_set* set = view_set(m_services);
		scn::world_context* found = set ? set->find(world) : nullptr;
		return found ? &found->state() : nullptr;
	});
	m_selection_connections = wire_selection(
		m_context->events(), m_context->selection(), m_state, m_scene_editor, m_world_ctrl,
		*m_hierarchy_panel, *m_inspector_panel,
		[this](ecs::entity ent) { m_viewport_panel->set_selected_entity(ent); });
	m_viewport_panel->set_selected_resolver([this] { return m_context->selection().entity(); });

	// Create prefab editor panel (EPIC-25)
	m_prefab_panel = std::make_shared<prefab_editor_panel>(m_rnd, m_gui);
	m_prefab_panel->set_on_close([this] { close_prefab_editor(); });
	m_prefab_panel->set_input_router(m_input_router);

	asset_kind_hooks asset_hooks;
	asset_hooks.open_prefab = [this](const res::tag& tag) { open_prefab_for_edit(tag); };
	m_data_documents = std::make_unique<data_document_host>(*m_context, m_services, [this] { return desc_layout(); }, [this](document& open) {
		if (m_context->documents().preview() == &open) m_prefab_panel->set_open(true);
	});
	m_data_documents->set_asset_source(asset_catalog);
	asset_hooks.add_desc_to_scene = [this](const res::tag& tag) {
		if (refused_during_play(m_play.get(), m_context->dialogs(), "Stop Play to add an asset to the scene.")) return;
		const std::string stem = std::filesystem::path(std::string(tag.relative())).stem().string();
		m_pending_desc_handle = m_services.resources()->require<desc::desc_base>(tag);
		m_pending_desc_tag = tag;
		m_pending_desc_key = m_scene_editor.make_unique_key(stem.empty() ? "Asset" : stem);
		editor_layer->register_implicit("edt/drop_loading", [this] { return poll_pending_desc(); });
	};
	for (auto& kind : register_builtin_asset_kinds(m_context->asset_kinds(), std::move(asset_hooks))) m_registrations.push_back(std::move(kind));
	m_asset_browser_panel->set_document_creator(
		std::make_shared<document_creator>(document_creator::ports{
			[this] { return m_services.descs(); },
			[this] { return m_services.writable_resources(); },
			[this] { return m_res.get_resource_layers(); },
			[this] { return project_write_blocker(); } }),
		[this](const res::tag& tag) { open_asset(tag, false); });
	m_asset_browser_panel->set_on_file_double_clicked([this](const std::filesystem::path& res_rel) {
		open_asset(res::tag::make(res_rel.generic_string()), false);
	});

	// Registration order is the render order and the View menu order.
	const auto add_panel = [this](const std::shared_ptr<panel_base>& panel, dock_slot slot, bool open_by_default) {
		m_registrations.push_back(m_context->panels().add({
			.id = panel->get_id(), .title = panel->get_title(), .slot = slot, .open_by_default = open_by_default, .panel = panel }));
	};
	add_panel(m_hierarchy_panel, dock_slot::left, true);
	add_panel(m_inspector_panel, dock_slot::right, true);
	add_panel(m_viewport_panel, dock_slot::center, true);
	m_game_view_panel = std::make_shared<game_view_panel>(m_rnd, m_gui);
	// The views show the set "play" while Play runs (view_set), the level otherwise.
	m_game_view_panel->set_registry_getter([this]() -> entt::registry* { return view_registry(m_services, m_state); });
	m_game_view_panel->set_world_name_getter([this] {
		const std::string* name = m_state.active_world_name();
		return name ? *name : std::string{};
	});
	m_game_view_panel->set_all_registries([this](const game_view_panel::registry_visitor& visit) {
		if (scn::context_set* set = view_set(m_services)) {
			set->for_each([&visit](scn::world_context& world) { visit(world.state()); });
		}
	});
	m_game_view_panel->set_input_router(m_input_router, [this] { return active_play_input_target(m_services, m_state); });
	add_panel(m_game_view_panel, dock_slot::center, cfg_show_game_view);
	// viewport.capture names the two views by what the panels show; the engine knows only these ids.
	if (data.has_value<rnd::capture_service>()) {
		auto& capture = data.require<rnd::capture_service>();
		m_registrations.push_back(capture.add_target("scene", [this] {
			return m_rnd.get_render_targets().current(viewport_panel::render_target_tag());
		}));
		m_registrations.push_back(capture.add_target("game", [this] {
			const auto& target = m_game_view_panel->render_target();
			return target ? m_rnd.get_render_targets().current(*target) : rnd::driver::texture_handle{};
		}));
	}
	add_panel(m_console_panel, dock_slot::bottom_right, true);
	m_problems_panel = std::make_shared<problems_panel>(m_context->problems(), data.has_value<prj::project_context>()
		? data.require<prj::project_context>().root : std::filesystem::path{});
	m_problems_panel->set_on_activate([this](const std::string& source, const problem& item) { go_to_problem(source, item); });
	// The idle gate of Edit > Undo (register_edit_actions): no dialog waiting, no gesture in progress.
	const auto problems_idle = [this] { return m_context->dialogs().pending() == 0 && !ImGui::IsAnyItemActive(); };
	// The asset fixes are the person's own: no command, so no agent reaches them.
	m_problems_panel->set_actions([this](const std::string& source, const problem& item) {
		std::vector<std::pair<std::string, std::function<void()>>> out;
		if (source != asset_problem_publisher::source || !m_assets || !m_asset_problems) {
			return out;
		}
		for (problem_action& action : m_asset_problems->actions_of(item, *m_assets)) {
			out.emplace_back(action.label, [label = action.label, run = std::move(action.run)] {
				if (const asset_fix_result result = run(); !result.ok) {
					egLOG_WARN("assets", "{}: {}", label, result.error);
				}
			});
		}
		return out;
	}, problems_idle);
	m_problems_panel->set_undo(
		[this]() -> std::optional<std::string> { return m_assets ? m_assets->next_undo_label() : std::nullopt; },
		[this] {
			if (!m_assets) {
				return;
			}
			if (const asset_fix_result result = m_assets->undo_last(); !result.ok) {
				egLOG_ERROR("assets", "{}", result.error);
			}
		},
		problems_idle);
	add_panel(m_problems_panel, dock_slot::bottom_right, true);
	m_blackboard_panel = std::make_shared<blackboard_panel>();
	m_blackboard_panel->set_manager_getter([this] { return m_services.ctx_manager.lock(); });
	add_panel(m_blackboard_panel, dock_slot::bottom_right, false);
	m_input_actions_panel = std::make_shared<input_actions_panel>();
	m_input_actions_panel->set_actions_getter([&data]() -> gs::input_actions* {
		return data.has_value<gs::input_actions>() ? &data.require<gs::input_actions>() : nullptr;
	});
	add_panel(m_input_actions_panel, dock_slot::bottom_right, false);
	add_panel(m_asset_browser_panel, dock_slot::bottom_left, true);
	add_panel(m_prefab_panel, dock_slot::floating, false);
	m_build_settings_panel = std::make_shared<build_settings_panel>([&data]() -> std::optional<std::filesystem::path> {
		if (!data.has_value<prj::project_context>()) return std::nullopt;
		return data.require<prj::project_context>().root;
	}, *cfg_build_configuration, [this] { return m_game_builder && m_game_builder->is_manual_build_running(); });
	// A live build is stopped instead: its configure would clear the --fresh mark that a define removal writes.
	m_build_settings_panel->set_stop_background([this] { stop_live_build(); });
	add_panel(m_build_settings_panel, dock_slot::floating, false);
	// #456: through the vcs.* commands as the person's calls; closed by default, so it reads nothing until opened.
	git_panel_model::ports git_ports;
	git_ports.invoke = [this](std::string_view id, boost::json::value args) { return m_context->actions().invoke(id, args); };
	git_ports.cancel = [this](std::uint64_t job) { return m_context->jobs().cancel(job); };
	m_git_panel = std::make_shared<git_panel>(m_context->events(), std::move(git_ports), m_context->dialogs(), m_res);
	add_panel(m_git_panel, dock_slot::right, false);
	world_systems_model::ports systems_ports;
	systems_ports.invoke = [this](std::string_view id, boost::json::value args) { return m_context->actions().invoke(id, args); };
	systems_ports.playing = [this] { return m_play && m_play->active(); };
	m_world_systems_panel = std::make_shared<world_systems_panel>(m_context->events(), std::move(systems_ports));
	add_panel(m_world_systems_panel, dock_slot::right, false);

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

	file_menu_hooks hooks;
	hooks.has_scene = [this] { return m_state.editor_tag.is_valid(); };
	hooks.is_dirty = [this] { return m_state.is_dirty(); };
	hooks.level_path = [this] {
		return m_state.level_tag.is_valid() ? std::string(m_state.level_tag.relative()) : std::string{};
	};
	hooks.new_level = [this] {
		if (m_play && m_play->active()) m_play->stop();
		cancel_level_settle();
		m_desc_problems.clear(m_context->problems(), "level");
		m_level_ctrl.new_level();
	};
	hooks.quick_save = [this] { return m_state.save().saved; };
	hooks.save_level_as = [this](const std::string& rel_path) {
		if (rel_path.find("://") != std::string::npos) {
			egLOG_ERROR("Editor/Save", "Level not saved: '{}' must be a path inside the project, without a protocol", rel_path);
			return false;
		}
		return m_state.save_as(res::tag::make(rel_path)).saved;
	};
	hooks.discard_changes = [this] { m_state.mark_clean(); };
	hooks.other_documents_dirty = [this] { return !m_data_documents->dirty().empty(); };
	hooks.save_other_documents = [this] { return m_data_documents->save_all(); };
	hooks.discard_other_documents = [this] {
		for (const document_id id : m_data_documents->dirty()) m_data_documents->close(id);
	};
	hooks.load_level = [this](const std::filesystem::path& abs_path) {
		const auto relative = abs_path.lexically_relative(m_res.get_resources_path());
		if (begin_level_load(res::tag::make(relative.string()))) {
			editor_layer->register_implicit("edt/load_level", [this] { return poll_level_load(); });
		}
	};
	// The commands an agent calls name a level by its project path: no file system path in between.
	hooks.load_level_tag = [this](const std::string& relative) -> std::optional<std::string> {
		if (!begin_level_load(res::tag::make(relative))) return "'" + relative + "' is not a level file";
		editor_layer->register_implicit("edt/load_level", [this] { return poll_level_load(); });
		return std::nullopt;
	};
	hooks.inspect_level = [this](const std::string& relative) { return inspect_level_in_layers(m_res, relative); };
	hooks.level_write_blocker = [this]() -> std::optional<std::string> {
		if (auto migrating = migration_blocker()) return migrating;
		if (m_loading_level) return std::string{ "a level is loading" };
		// Whatever lifted a migration's block: a detached level has no worlds, so a save would write an empty level (#761).
		std::optional<std::string> stopped = m_migration_stopped.reason();
		if (m_level_ctrl.is_detached()) return stopped ? *stopped : std::string{ "the level is detached: it is not loaded from disk again yet" };
		return stopped;
	};
	// An agent's file.open / file.new would replace the start level behind the loading screen, and Play would start on
	// its level (#518). A person has no menu or shortcut then.
	hooks.level_change_blocker = [this]() -> std::optional<std::string> {
		if (auto migrating = migration_blocker()) return migrating;
		if (m_modes && m_modes->startup_pending()) return std::string{ "the start level is loading" };
		if (m_loading_level) return std::string{ "a level is loading" };
		return std::nullopt;
	};
#if SNK_HAS_ASSIMP
	hooks.import_model = [this](const std::filesystem::path& abs_path) { start_import(abs_path); };
#endif
	hooks.agent_import = m_agent_import.get();
	hooks.exit = m_exit_action;
	hooks.exit_blocker = [this] { return migration_blocker(); };
	hooks.resources_path = m_res.get_resources_path();
	hooks.open_project = start_editor_on;
	// From disk, not through res::: a recursive copy into a folder that no mount covers yet.
	hooks.create_project = [](const std::filesystem::path& folder, const std::string& name) {
		// The template ships without .resource sidecars: each new project gets ids of its own (#765).
		return prj::create_project(*cfg_edt_data_path / "templates" / "projects" / "empty", folder, name,
			[](const std::filesystem::path& staging) { return edt::give_template_fresh_ids(staging); });
	};
	// Next to the open project, else where the editor was started.
	hooks.projects_path = data.has_value<prj::project_context>() ? data.require<prj::project_context>().root.parent_path()
		: std::filesystem::current_path();
	hooks.recent_projects = edt::recent_projects_besides(m_res, data.has_value<prj::project_context>()
		? std::optional<std::filesystem::path>{ data.require<prj::project_context>().manifest_file } : std::nullopt);
	m_file_menu = std::make_unique<file_menu>(m_context->dialogs(), m_context->actions(), std::move(hooks));
	for (auto& menu : register_builtin_menus(m_context->actions())) m_registrations.push_back(std::move(menu));
	m_file_menu->register_actions();
	m_registrations.push_back(m_context->actions().add({ .id = "level.cancel_load", .label = "Cancel Level Load",
		.menu_path = "File", .separator_before = true,
		// While nothing loads the registry answers `disabled` with this reason: a cancel that comes after the load ended changes nothing.
		.disabled_reason = [] { return std::string{ "no level is loading" }; },
		.execute_call = [this](const cmd::call& call) { return cancel_level_load(call.from); },
		.result_schema = boost::json::parse(R"json({
			"type": "object",
			"properties": { "cancelled": { "type": "boolean" }, "level": { "type": "string" } },
			"required": ["cancelled", "level"],
			"additionalProperties": false
		})json"),
		.enabled = [this] { return m_loading_level; },
		.category = cmd::agent_category::scene_edit }));
	const auto asset_undo_label = [this]() -> std::optional<std::string> { return m_assets ? m_assets->next_undo_label() : std::nullopt; };
	const auto asset_undo = [this] {
		if (!m_assets) {
			return false;
		}
		const asset_fix_result result = m_assets->undo_last();
		if (!result.ok) {
			egLOG_ERROR("assets", "{}", result.error);
		}
		return result.ok;
	};
	for (auto& action : register_edit_actions(m_context->actions(), m_context->documents(), m_context->dialogs(), [] { return ImGui::IsAnyItemActive(); },
			asset_undo_label, asset_undo)) {
		m_registrations.push_back(std::move(action));
	}
	// A5 (#291): the agent's edits of the scene. Offered over MCP by the agent policy (scene_edit: allowed by default).
	for (auto& command : register_scene_commands(m_context->actions().commands(),
			{ m_context->documents(), m_context->selection(), m_services, [this] { return person_is_editing(); } })) {
		m_registrations.push_back(std::move(command));
	}
	m_agent_sessions = std::make_unique<agent_sessions>(m_context->documents());
	for (auto& command : m_agent_sessions->register_commands(m_context->actions().commands())) {
		m_registrations.push_back(std::move(command));
	}
	// A7 (#451): the agent's edits of code/ through code://; without a code folder they answer no_project.
	for (auto& command : register_code_commands(m_context->actions().commands(), { m_res, &m_context->events() })) {
		m_registrations.push_back(std::move(command));
	}
	// #550: the agent's reads and writes of the desc files of res/, checked against the schemas first.
	{
		edt::desc_file_deps desc_files{ m_res, desc_system };
		desc_file_layout layout = desc_layout();
		desc_files.top_layer = std::move(layout.top_layer);
		desc_files.layers = std::move(layout.layers);
		desc_files.open_level_files = std::move(layout.open_level_files);
		desc_files.write_blocker = std::move(layout.write_blocker);
		desc_files.events = &m_context->events();
		for (auto& command : register_desc_file_commands(m_context->actions().commands(), std::move(desc_files))) {
			m_registrations.push_back(std::move(command));
		}
	}
	// EPIC-58 S7 (#759): project.migration_plan and project.migrate (in place, on the main thread). Refused while Play, a load,
	// a build or a module reload, an import, the export dialog or a git write runs (busy), and with an uncommitted edit or
	// any dirty document (needs_input). The migration closes every document but the scene, lets go of the level, writes,
	// then attaches the level and loads it again from disk.
	{
		migration_deps migration{ m_context->jobs() };
		migration.project_root = [&data]() -> std::optional<std::filesystem::path> {
			if (!data.has_value<prj::project_context>()) return std::nullopt;
			return data.require<prj::project_context>().root;
		};
		migration.project_name = [&data] {
			return data.has_value<prj::project_context>() ? data.require<prj::project_context>().data.name : std::string{};
		};
		migration.content_roots = [&data] {
			return data.has_value<prj::project_context>() ? data.require<prj::project_context>().content_roots : std::vector<std::filesystem::path>{};
		};
		migration.descs = [this]() -> desc::desc_system& { return *m_services.descs(); };
		migration.resources = [this]() -> res::resource_system& { return *m_services.resources(); };
		migration.busy_reason = [this]() -> std::optional<std::string> {
			if (m_play && m_play->active()) return std::string{ "Play is running: stop it first" };
			if (m_modes && m_modes->startup_pending()) return std::string{ "the start level is loading" };
			if (auto loading = load_in_progress()) return loading;
			if (m_game_builder && m_game_builder->is_building()) return std::string{ "a game build is running" };
			if (editor_layer->has_implicit_prefix(game_reloader::k_reload_frame_id) || editor_layer->has_implicit_prefix(live_mode::k_reload_frame_id)) {
				return std::string{ "a reload of the game module is waiting" };
			}
			if (m_agent_import && m_agent_import->running()) return std::string{ "a model import asked for by an agent is running" };
			if (m_export_dialog_active) return std::string{ "the asset export dialog is open" };
			if (m_vcs && m_vcs->running_writes() > 0) return std::string{ "a git operation (pull, checkout, commit...) is running" };
#if SNK_HAS_ASSIMP
			if (m_importing) return std::string{ "a model import is running" };
#endif
			return std::nullopt;
		};
		migration.unsaved_reason = [this]() -> std::optional<std::string> {
			if (person_is_editing()) return std::string{ "a person is editing a value (an inspector field or a gizmo drag): commit it first" };
			if (m_state.is_dirty()) return std::string{ "the level has unsaved changes: save or discard them first" };
			std::optional<std::string> dirty;
			m_context->documents().for_each([&dirty](document& open) {
				if (!dirty && open.is_dirty()) dirty = std::format("'{}' has unsaved changes: save or discard them first", open.title());
			});
			return dirty;
		};
		migration.close_documents_and_detach_level = [this] {
			close_prefab_editor();
			// Every document but the scene: the prefab editor through its panel, data documents through their host, any other
			// (a module's, an extension's) through document_manager::close, which announces it to whoever shows it.
			document_manager& documents = m_context->documents();
			std::vector<document_id> open_documents;
			documents.for_each([&documents, &open_documents](document& open) {
				if (&open != &documents.scene()) open_documents.push_back(open.id());
			});
			for (const document_id id : open_documents) {
				if (m_data_documents->owns(id)) m_data_documents->close(id);
				else if (documents.find(id)) documents.close(id);
			}
			const res::tag level = current_level_tag();
			// As follow_game_layer_change before a layer change: nothing may hold the level while its files are written.
			m_pending_desc_handle = {};
			if (m_loading_level) abandon_level_load();
			// A level a stopped migration left detached stays as it was kept (a second detach would keep the empty worlds).
			if (!m_level_ctrl.is_detached()) m_level_ctrl.detach_from_game_layer();
			return level;
		};
		migration.pause_watch = [this] { m_level_ctrl.unwatch_level(); };
		// Through the base system: it reaches the game layer's too.
		migration.signal_changed = [this](const res::tag& tag) { m_res.signal_changed(tag); };
		migration.rebuild_assets = [this] {
			if (m_assets) m_assets->rebuild();
		};
		migration.reopen_level = [this](const res::tag& level, std::function<void(bool)> loaded) -> std::optional<std::string> {
			// The files are still half migrated (a rollback refused): the level stays detached, as the stopped migration left it.
			if (std::optional<std::string> stopped = m_migration_stopped.reason()) return stopped;
			m_level_ctrl.attach_to_game_layer();
			if (!level.is_valid()) return std::nullopt;
			if (!begin_level_load(level)) return "'" + std::string(level.view()) + "' did not start loading again";
			editor_layer->register_implicit("edt/load_level", [this, loaded = std::move(loaded)] {
				bool done = false;
				const bool loading = poll_level_load(&done);
				if (!loading && loaded) loaded(done);
				return loading;
			});
			return std::nullopt;
		};
		migration.level_load_in_progress = [this] { return m_loading_level; };
		migration.approved_internally = [this](std::string_view ticket) {
			return m_agent_access != nullptr && m_agent_access->confirmations().approved_internally(ticket);
		};
		migration.code_identity = [this] { return m_game_layer_generation; };
		migration.level_left_detached = [this](const res::tag& level, std::function<bool()> rolled_back, const std::string& reason) {
			static_cast<void>(level);
			m_migration_stopped.hold(std::move(rolled_back),
				"a migration of the project stopped half way: roll the migration back first (" + reason + ")");
		};
		migration.current_level_tag = [this] { return current_level_tag(); };
		migration.register_implicit = [this](std::string_view id, std::function<bool()> fn) { editor_layer->register_implicit(id, std::move(fn)); };
		migration.running_changed = [this](bool running) {
			m_migration_running = running;
			if (running) ++m_migration_runs;
		};
		// The save block of a stopped migration goes with its rollback (its rolled_back() sees the journal too).
		migration.migration_rolled_back = [this](const std::string&) {
			static_cast<void>(m_migration_stopped.reason());
		};
		migration.show_progress = [](bool open) {
			constexpr const char* title = "Migrating Project##edt_migrate";
			ImGui::OpenPopup(title);
			if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar)) {
				if (open) {
					ImGui::Text("Migrating project...");
				} else {
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		};
		for (auto& command : register_migration_commands(m_context->actions().commands(), std::move(migration))) {
			m_registrations.push_back(std::move(command));
		}
	}
	// EPIC-58 S8 (#761): Project > Migrate Project... opens the Project Migration window, which runs the two commands above as
	// a person's calls (origin ui) and takes the migration's answer from job_finished.
	{
		migration_window_model::ports ports;
		ports.invoke = [this](std::string_view id, boost::json::value args) { return m_context->actions().invoke(id, args); };
		ports.open_file = [this, &data](const std::string& path) {
			if (!data.has_value<prj::project_context>()) return;
			const prj::project_context& project = data.require<prj::project_context>();
			const std::filesystem::path file = (project.root / fs_paths::path_from_utf8(path)).lexically_normal();
			for (const std::filesystem::path& content : project.content_roots) {
				const std::filesystem::path relative = file.lexically_relative(content.lexically_normal());
				if (relative.empty() || *relative.begin() == std::filesystem::path("..")) continue;
				if (!open_asset(res::tag::make(fs_paths::generic_utf8_of(relative)), false)) {
					egLOG_INFO("project/migrate", "'{}' is not a file the editor opens", path);
				}
				return;
			}
			egLOG_INFO("project/migrate", "'{}' is not a file the editor opens", path);
		};
		m_migration_panel = std::make_shared<migration_panel>(m_context->events(), std::move(ports));
		add_panel(m_migration_panel, dock_slot::floating, false);
		m_registrations.push_back(register_migration_window_action(m_context->actions(),
			[&data] { return data.has_value<prj::project_context>(); }, [this] { open_migration_window(); }));
	}
	m_file_menu->attach_window_close(wnd_sys);

	game_builder_options build_options;
	if (data.has_value<prj::project_context>()) {
		const auto& project = data.require<prj::project_context>();
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
	build_options.default_configuration = *cfg_build_configuration;
	build_options.toolchain = { *cfg_build_cmake, *cfg_build_vs_path, *cfg_build_vs_min_version };
	build_options.show_progress = [this](std::string_view id, std::function<bool()> draw) {
		editor_layer->register_implicit(id, std::move(draw));
	};
	build_options.events = &m_context->events();
	build_options.auto_jobs = [] { return estimate_jobs().jobs; };
	build_options.start_blocker = [this] { return migration_blocker(); };
	m_game_builder = std::make_unique<game_builder>(m_context->jobs(), m_context->problems(), m_context->dialogs(),
		m_context->actions(), std::move(build_options));

	// The host appears in the storage after init (main loads the project's modules then): it is looked up when used.
	game_reloader_options reload_options;
	reload_options.modules = [&data] {
		return data.has_value<prj::project_context>() ? project_module_names(data.require<prj::project_context>()) : std::vector<std::string>{};
	};
	reload_options.is_editor_module = [&data](const std::string& module) {
		return data.has_value<core::project_module_host>() && data.require<core::project_module_host>().is_editor_module(module);
	};
	reload_options.is_current = [&data](const std::string& module) {
		return data.has_value<core::project_module_host>() && data.require<core::project_module_host>().is_current(module);
	};
	// The action may be asked for from a panel of an editor module: whatever unloads editor modules waits for the next UI frame.
	reload_options.unloads_editor_modules = [&data](const std::string& module) {
		if (!data.has_value<core::project_module_host>()) return false;
		const auto& host = data.require<core::project_module_host>();
		return host.is_editor_module(module) || host.has_editor_modules();
	};
	reload_options.build_output = [&data](const std::string& module) -> std::optional<std::filesystem::path> {
		if (!data.has_value<core::project_module_host>()) return std::nullopt;
		return data.require<core::project_module_host>().build_output(module);
	};
	const auto reload_module = [this, &data](const std::string& module, bool retry_taken_back) {
		if (data.has_value<core::project_module_host>() && data.require<core::project_module_host>().is_editor_module(module)) {
			// An editor module changes neither the game layer nor Play: no swap gate, no snapshot, nothing to revert.
			return data.require<core::project_module_host>().reload_editor_module(module);
		}
		core::reload_report report = data.has_value<core::project_module_host>()
			? data.require<core::project_module_host>().reload(module, m_swap_gate.hooks(), retry_taken_back)
			: core::reload_report{ core::reload_status::refused, "the editor did not load the project's modules" };
		// A snapshot before_reload took and no swap used goes, whatever the outcome.
		if (m_play) m_play->end_reload();
		// The build loaded but could not restore Play: it is taken back next frame, so it is not reported as running.
		if (report.status == core::reload_status::reloaded && m_play && m_play->reverting()) {
			report.status = core::reload_status::rolled_back;
			report.message = std::format("it could not restore Play ({}); it is being taken back, the previous build loads again",
				m_play->restore_failure());
		}
		return report;
	};
	reload_options.reload = [reload_module](const std::string& module) { return reload_module(module, false); };
	// Only a person loads a build the host took back again (#601): live mode would loop.
	reload_options.reload_by_person = [reload_module](const std::string& module) { return reload_module(module, true); };
	// Play keeps its world across the swap (#600): the snapshot is taken after every check, right before it.
	reload_options.before_reload = [this](const std::string&) { return m_play ? m_play->capture_for_reload() : std::string{}; };
	reload_options.building = [this] { return m_game_builder && m_game_builder->is_manual_build_running(); };
	reload_options.background_building = [this] { return m_game_builder && m_game_builder->building_preemptible(); };
	reload_options.stop_background = [this] { stop_live_build(); };
	reload_options.events = &m_context->events();
	reload_options.next_frame = [this](const std::string& id, std::function<void()> fn) {
		editor_layer->register_implicit(id, [fn = std::move(fn)] {
			fn();
			return false;
		});
	};
	// Reload during a live build: waits under edt/reload_game_module/<module>, which also holds live mode's next build.
	reload_options.each_frame = [this](const std::string& id, std::function<bool()> poll) {
		editor_layer->register_implicit(id, std::move(poll));
	};
	reload_options.defer = [this] {
		if (auto migrating = migration_blocker()) return *migrating;
		if (auto loading = load_in_progress()) return *loading;
		if (const auto contexts = m_services.ctx_manager.lock(); contexts && contexts->level_switch_pending()) {
			return std::string{ "the game is switching to another level" };
		}
		// A second reload waits for the first one's restore (#600): its snapshot would see the set held in Pause.
		if (m_play && m_play->reloading()) return std::string{ "Play is loading the resources of the last reload" };
		return std::string{};
	};
	m_game_reloader = std::make_unique<game_reloader>(m_context->dialogs(), m_context->actions(), std::move(reload_options));

	create_live_mode(data);
	create_play_session(data);

	m_registrations.push_back(m_context->actions().add({ .id = "build.settings.open", .label = "Build Settings...",
		.menu_path = "Build", .separator_before = true,
		.execute = no_args([this] {
			m_context->panels().set_open(m_build_settings_panel->get_id(), true);
			m_build_settings_panel->focus();
		}),
		.expose_as_tool = false, .category = cmd::agent_category::not_for_agents }));
	build_settings_command_options settings_command;
	settings_command.project_root = [&data]() -> std::optional<std::filesystem::path> {
		if (!data.has_value<prj::project_context>()) return std::nullopt;
		return data.require<prj::project_context>().root;
	};
	settings_command.default_configuration = *cfg_build_configuration;
	for (auto& command : register_build_settings_commands(m_context->actions().commands(), std::move(settings_command))) {
		m_registrations.push_back(std::move(command));
	}

	vcs_options vcs;
	vcs.git = *cfg_vcs_git;
	vcs.project_root = [&data]() -> std::optional<std::filesystem::path> {
		if (!data.has_value<prj::project_context>()) return std::nullopt;
		return data.require<prj::project_context>().root;
	};
	m_vcs = std::make_unique<vcs_service>(m_context->jobs(), std::move(vcs));
	edt::vcs::repository_watcher::ports watch_ports;
	watch_ports.own_writes_running = [this] { return m_vcs && m_vcs->running_writes() > 0; };
	m_repo_watcher = std::make_unique<edt::vcs::repository_watcher>(std::move(watch_ports));
	m_vcs->set_write_observer([this] {
		if (m_repo_watcher) m_repo_watcher->begin_own_write();
	});
	if (data.has_value<prj::project_context>()) {
		const prj::project_context& project = data.require<prj::project_context>();
		m_assets = std::make_unique<asset_database>(m_context->jobs(), &m_context->events(), m_res.get_resource_layers(),
			asset_cache_location{ project.root, project.cache_dir().lexically_relative(project.root) / "asset_db" });
		m_assets->attach(m_res);
		m_assets->set_vcs_busy([this] { return m_vcs && m_vcs->running_writes() > 0; });
		m_assets->set_git_dir_provider([this]() -> std::optional<std::filesystem::path> {
			return m_vcs ? m_vcs->cached_state().git_dir : std::nullopt;
		});
		m_assets->rebuild();
		m_asset_catalog = std::make_unique<picker::database_catalog>(*m_assets);
		m_asset_index = std::make_unique<asset_index_link>(*m_assets, m_res);
		m_asset_problems = std::make_unique<asset_problem_publisher>();
	}
	for (auto& command : register_vcs_commands(m_context->actions().commands(), *m_vcs)) {
		m_registrations.push_back(std::move(command));
	}
	forge_options forge_setup;
	forge_setup.provider = std::make_shared<forge::github_provider>(forge::make_curl_transport(forge::configured_curl_options()));
	m_secrets = std::make_shared<platform::secret_store>(platform::make_os_secret_backend());
	forge_setup.secrets = m_secrets;
	const std::vector<std::string> forge_hosts = forge_setup.allowed_hosts;
	m_forge = std::make_unique<forge_service>(*m_vcs, std::move(forge_setup));
	for (auto& command : register_forge_commands(m_context->actions().commands(), *m_forge)) {
		m_registrations.push_back(std::move(command));
	}
	// #458 PR-4: where a person sets the token of each host. The secret store's calls can block on an OS prompt, so they
	// run as jobs on the io lane; the job holds its own copies, nothing of it touches the panel.
	forge_account_model::ports account_ports;
	account_ports.secrets = m_secrets;
	account_ports.start_job = [this](std::string label, std::function<boost::json::value()> work) -> std::optional<std::uint64_t> {
		return m_context->jobs().start_with_result(ds::lane::io, ds::task_desc{ std::move(label), "forge" }, m_forge_lifetime,
			[work = std::move(work)](ds::task_context&) -> boost::json::value { return work(); });
	};
	m_forge_account_panel = std::make_shared<forge_account_panel>(m_context->events(), forge_hosts, std::move(account_ports));
	add_panel(m_forge_account_panel, dock_slot::floating, false);
	const auto open_forge_account = [this] {
		m_context->panels().set_open(m_forge_account_panel->get_id(), true);
		m_forge_account_panel->focus();
	};
	m_git_panel->set_open_account(open_forge_account);
	m_git_panel->set_describe_target([this](const std::string& remote) { return m_forge->describe_target(remote); });
	m_registrations.push_back(register_forge_account_action(m_context->actions(), open_forge_account));

	// A project held back for its engine version: nothing of it is mounted, and no start level is loaded either.
	const bool project_held = data.has_value<prj::held_project>();
	if (project_held) {
		auto& held = data.require<prj::held_project>();
		// Open Anyway, and Migrate... (#761): the same restart, the new editor opening the Project Migration window.
		const auto restart_on = [this, manifest = held.manifest_file](bool open_migration) {
			const auto outcome = m_context->actions().invoke("file.open_project", boost::json::object{
				{ "path", utf8_of(manifest) }, { "allow_engine_version_mismatch", true }, { "discard_unsaved", true },
				{ "open_migration", open_migration } });
			if (!outcome.ok) {
				m_context->dialogs().message(message_level::error, std::format("The project was not opened: {}.", outcome.error));
			}
		};
		ask_about_held_project(m_context->dialogs(), held, m_exit_action, [restart_on] { restart_on(false); },
			[restart_on] { restart_on(true); });
	}

	std::string startup_level = scn::start_level_name(data, *cfg_startup_level);
	const std::optional<editor_mode> start_mode = editor_mode_of(*cfg_start_mode);
	if (!start_mode) {
		egLOG_WARN("editor", "editor/start_mode '{}' is neither 'editor' nor 'game': the editor starts in editor mode", *cfg_start_mode);
	}
	const bool start_in_game = start_mode == editor_mode::game;
	if (start_in_game && !cfg_game_level->empty()) {
		if (const std::string problem = game_level_problem(*cfg_game_level); problem.empty()) {
			startup_level = *cfg_game_level;
		} else {
			// Never read as a file path; and no other level stands in for the one asked for.
			egLOG_ERROR("editor", "--game='{}' is not taken: {}; no level is loaded at startup", *cfg_game_level, problem);
			startup_level.clear();
		}
	}
	if (start_in_game && (project_held || startup_level.empty())) {
		egLOG_WARN("editor", "game mode needs a start level (--game=<level>, editor/startup_level or the project's start "
			"scene) and none is loaded: the editor starts in editor mode");
	}
	if (!project_held && !startup_level.empty()) {
		// On the first frame: the host loads the project's game modules after init (#428), and the level may use their types.
		const res::tag level = scn::start_level_tag(startup_level);
		// #518: a neutral loading screen until the load ends; every end of it (loaded, failed, Esc in its modal) passes
		// through the poll below to finish_startup.
		if (start_in_game && m_modes) m_modes->begin_startup(std::string(level.view()));
		editor_layer->register_implicit("edt/load_level", begin_then_poll([this, level] { static_cast<void>(begin_level_load(level)); },
			[this, start_in_game] {
				bool loaded = false;
				if (poll_level_load(&loaded)) {
					return true;
				}
				// Begun in a frame, the load ends a frame later, and closing its modal focuses the top panel. The
				// editor starts with no panel focused, as when the load began before the first frame (#437).
				ImGui::SetWindowFocus(nullptr);
				if (start_in_game && m_modes) m_modes->finish_startup(loaded, static_cast<bool>(m_stop_after_level_loaded));
				return false;
			}));
	}

	// #761: a migration the project was left with is offered for a rollback once the start level settled; headless nobody
	// is asked (a WARN names the command line), and nothing waits for an answer.
	if (!project_held && data.has_value<prj::project_context>()) {
		const std::filesystem::path root = data.require<prj::project_context>().root;
		migration_startup::ports startup_ports;
		startup_ports.settled = [this] {
			return !(m_loading_level || m_level_settle.has_value() || (m_modes && m_modes->startup_pending())
				|| editor_layer->has_implicit_prefix("edt/load_level"));
		};
		startup_ports.unfinished = [root] { return prj::migration::unfinished_migrations(root); };
		startup_ports.warn = [](const std::string& text) { egLOG_WARN("project/migrate", "{}", text); };
		startup_ports.ask = [this](const journal_offer& offer) { offer_migration_rollback(offer); };
		startup_ports.start_scan = [this, root] { start_migration_scan(root); };
		auto startup = std::make_shared<migration_startup>(std::move(startup_ports), headless);
		// Headless: the WARN only, now in init (no question, no scan). With a person never in init, even with nothing to
		// wait for: the project's game modules load after it (#428), and the scan's types are theirs.
		if (headless) static_cast<void>(startup->tick());
		else editor_layer->register_implicit("edt/migration_journal", [startup] { return startup->tick(); });
	}

	if (cfg_open_migration && !headless && !project_held && data.has_value<prj::project_context>()) {
		editor_layer->register_implicit("edt/open_migration", [this] {
			const bool settling = m_loading_level || m_level_settle.has_value() || (m_modes && m_modes->startup_pending())
				|| editor_layer->has_implicit_prefix("edt/load_level");
			if (settling) return true;
			open_migration_window();
			return false;
		});
	}

	bool stdio = cfg_mcp_stdio;
	if (stdio && !data.has_value<platform::protocol_stdout>()) {
		egLOG_ERROR("mcp", "editor/mcp_stdio needs the --mcp-stdio option (stdout must belong to the protocol): no stdio transport");
		stdio = false;
	}
	{
		// The policy and the server object always exist, so the person can switch the HTTP transport on later (#449). The
		// transport itself runs when cfg `editor/agent_server` says so (it wins, with its port) or the person's own switch
		// (edt://settings/agent_server.json) is on.
		agent_server_options options;
		const auto user_roots_for_server = m_res.mount_roots("edt");
		const std::filesystem::path settings_dir = (user_roots_for_server.empty() ? default_user_data_dir() : user_roots_for_server.back()) / "settings";
		m_agent_server_settings_file = settings_dir / "agent_server.json";
		const agent_server_settings person_choice = load_agent_server_settings(m_agent_server_settings_file);
		const std::string cfg_text = cfg_agent_server;
		const int cfg_port = cfg_agent_server_port;
		const agent_http_choice http = decide_agent_http(parse_cfg_switch(cfg_text), cfg_port, person_choice, headless, stdio);
		options.http = http.enabled;
		options.port = http.port;
		const int idle_minutes = cfg_agent_session_idle_minutes;
		// More than a week is the same as never, and keeps the minutes far from overflowing the clock's nanoseconds.
		constexpr int never_after_minutes = 7 * 24 * 60;
		options.session_idle_timeout = std::chrono::minutes{ (idle_minutes > never_after_minutes) ? 0 : std::max(idle_minutes, 0) };
		const std::string project_dir = cfg_project_dir;
		const auto agent_roots = m_res.mount_roots("edt");
		options.project_dir = agent_project_dir(project_dir, data.has_value<prj::project_context>()
			? std::optional<std::filesystem::path>{ data.require<prj::project_context>().root } : std::nullopt,
			agent_roots.empty() ? default_user_data_dir() : agent_roots.back());
		const std::filesystem::path project_policy = options.project_dir / ".snk" / "agent_policy.json";
		const auto user_roots = m_res.mount_roots("edt");
		const std::filesystem::path user_dir = user_roots.empty() ? default_user_data_dir() : user_roots.back();
		auto& commands = data.require<cmd::command_registry>();
		commands.set_agent_argument_check(make_agent_model_check(m_res));
		m_agent_argument_checks = &commands;
		m_agent_access = std::make_unique<agent_access>(commands, project_policy, user_dir / "settings" / "agent_policy.json", agent_confirmations::clock_function{},
			approved_snapshot_for(user_dir / "settings", options.project_dir));
		// Asked at the start of every agent write, so an edit of either policy file applies at once.
		m_vcs->set_policy_file(project_policy);
		m_vcs->set_policy_source([this] { return m_agent_access != nullptr ? m_agent_access->policy().effective_vcs() : vcs_policy{}; });
		m_forge->set_policy_source([this] { return m_agent_access != nullptr ? m_agent_access->policy().effective_vcs() : vcs_policy{}; });
		m_agent_access->policy().set_on_changed([this] {
			if (m_agent_server) m_agent_server->notify_tools_changed();
		});
		m_agent_access->policy().set_live_mode([this] { return m_live_mode != nullptr && m_live_mode->enabled(); });
		m_agent_access->set_questions_routed(true);
		// A person is here to approve a looser project policy only with a window; headless never loosens (#483).
		m_agent_access->policy().set_approval_allowed(!headless);
		// Once, so a project file that loosens something is known (the notice, the status bar) before an agent ever connects.
		if (!headless) m_agent_access->policy().refresh();
		m_agent_access->set_headless(headless);
		if (const std::filesystem::path preanswers = *cfg_agent_policy_file; !preanswers.empty()) {
			if (headless) {
				m_agent_access->policy().set_preanswers_file(preanswers, options.project_dir);
			} else {
				egLOG_WARN("agent", "--agent-policy ignored: not headless ({})", utf8_of(preanswers));
			}
		}
		// Headless nobody reads a question, so the UI font is not checked (it has no Cyrillic there).
		editor_layer->register_implicit("edt/agent_confirmations", [this, headless] {
			if (m_agent_access == nullptr) return false;
			if (m_agent_server) m_agent_server->tick();
			// Nothing to do for a person who does not use agents: no question can come, and the policy files are not read.
			if (m_agent_server == nullptr || !m_agent_server->active()) return true;
			return headless ? m_agent_access->frame_headless() : m_agent_access->frame(ImGui::GetFont());
		});
		// One signal for the end of an MCP session; whoever keeps something per session listens to it.
		editor_events& events = m_context->events();
		m_session_connections.push_back(events.agent_session_closed.subscribe([this](const std::string& session) {
			if (m_agent_access) m_agent_access->confirmations().drop_session(session);
		}));
		m_session_connections.push_back(events.agent_session_closed.subscribe([this](const std::string& session) {
			if (m_agent_sessions) m_agent_sessions->session_closed(session);
		}));
		options.on_session_closed = [&events](const std::string& session) { events.agent_session_closed.emit(session); };
		options.jobs = &m_context->jobs();
		options.events = &events;
		editor_quit_sources quit;
		quit.stdio_session = [this] { return m_agent_server ? m_agent_server->stdio_session() : std::string{}; };
		// Every open document, not only the scene: a modified preview is dropped too.
		quit.is_dirty = [this] {
			return any_unsaved([this](const std::function<void(document&)>& visit) { m_context->documents().for_each(visit); });
		};
		quit.discard_changes = [this] {
			m_state.mark_clean();
			auto& documents = m_context->documents();
			if (prefab_document* preview = prefab_of(documents); preview != nullptr && preview->is_dirty()) {
				documents.close(preview->id());
			}
		};
		quit.request_stop = [&data] { data.require<app::app_loop_service_interface>().request_stop(); };
		if (stdio) {
			options.stdio_write = [fd = data.require<platform::protocol_stdout>().fd](std::string_view line) {
				return platform::write_all(fd, line);
			};
			auto& tasks = data.require<ds::task_system>();
			options.on_stdio_eof = [&tasks, alive = std::weak_ptr<int>(m_stdio_alive), headless, quit] {
				tasks.post_main([alive, headless, quit] {
					if (alive.lock()) on_stdio_input_closed(headless, quit);
				});
			};
		}
		m_agent_server = std::make_unique<agent_server>(commands, data.require<ds::task_system>(), *m_agent_access, std::move(options));
		if (editor_quit_available(stdio, headless)) {
			registration added = register_editor_quit(commands, std::move(quit));
			if (added.active()) m_registrations.push_back(std::move(added));
		}

		// The agents window (#449). Nothing here is a command an agent could call: the buttons call the model directly.
		m_agent_policy_model = std::make_unique<agent_policy_model>(m_agent_access->policy(), [&commands] { return commands.list(); });
		agents_panel_services services;
		services.server = [this] { return m_agent_server.get(); };
		services.set_server = [this](bool enabled, std::uint16_t port) { return set_agent_server(enabled, port); };
		services.chosen_port = [this] { return load_agent_server_settings(m_agent_server_settings_file).port; };
		m_agents_panel = std::make_shared<agents_panel>(*m_agent_policy_model, std::move(services));
		m_registrations.push_back(m_context->panels().add({ .id = m_agents_panel->get_id(), .title = m_agents_panel->get_title(),
			.slot = dock_slot::floating, .open_by_default = false, .panel = m_agents_panel }));
		m_registrations.push_back(m_context->actions().add({ .id = "agent.settings.open", .label = "Agents...",
			.execute = no_args([this] { open_agents_panel(); }),
			.expose_as_tool = false, .category = cmd::agent_category::not_for_agents }));
		m_session_connections.push_back(events.agent_action.subscribe([this](const agent_action_info& info) {
			m_recent_agent_actions.push_back({ info.command, info.target, info.session, std::chrono::steady_clock::now() });
			while (m_recent_agent_actions.size() > 8) m_recent_agent_actions.pop_front();
		}));
		editor_layer->register_implicit("edt/agent_policy_notice", [this, headless] {
			if (!headless) draw_agent_policy_notice();
			return true;
		});
	}
}

bool edt::editor_system::set_agent_server(bool enabled, std::uint16_t port)
{
	if (!m_agent_server) {
		return false;
	}
	bool done = true;
	if (enabled) {
		done = m_agent_server->start_http(port);
	}
	else {
		m_agent_server->stop_http();
	}
	if (!done) {
		return false;
	}
	if (const std::optional<std::string> error = save_agent_server_settings(m_agent_server_settings_file, agent_server_settings{ enabled, port })) {
		egLOG_WARN("editor/agent_server", "the agent server choice cannot be saved: {}", *error);
	}
	return true;
}

void edt::editor_system::create_live_mode(ds::app_data_storage& data)
{
	const std::optional<std::filesystem::path> snk_dir = data.has_value<prj::project_context>()
		? std::optional<std::filesystem::path>{ data.require<prj::project_context>().snk_dir() } : std::nullopt;
	const bool has_code = !m_res.mount_roots(prj::k_code_protocol).empty();

	live_mode_options live;
	live.build.settings = m_game_builder->options().settings;
	live.build.make_request = [this](const build_settings& settings) {
		return make_live_build_request(settings, m_game_builder->options());
	};
	live.build.start_build = [this](build_request request) { return m_game_builder->start(std::move(request)); };
	live.build.is_building = [this] { return m_game_builder->is_building(); };
	live.build.stop_background = [this] { m_game_builder->stop_background(); };
	// The host appears in the storage after init, like for game_reloader: looked up when used.
	live.build.is_loaded_current = [&data](const std::string& module) {
		return data.has_value<core::project_module_host>() && data.require<core::project_module_host>().is_current(module);
	};
	live.build.modules = [&data] {
		return data.has_value<prj::project_context>() ? project_module_names(data.require<prj::project_context>()) : std::vector<std::string>{};
	};
	live.build.is_editor_module = [&data](const std::string& module) {
		return data.has_value<core::project_module_host>() && data.require<core::project_module_host>().is_editor_module(module);
	};
	live.build.reload_pending = [this] {
		return editor_layer->has_implicit_prefix(game_reloader::k_reload_frame_id) || editor_layer->has_implicit_prefix(live_mode::k_reload_frame_id);
	};
	live.events = &m_context->events();
	live.next_frame = [this](const std::string& id, std::function<void()> fn) {
		editor_layer->register_implicit(id, [fn = std::move(fn)] {
			fn();
			return false;
		});
	};
	live.reload_module = [this](const std::string& module, std::function<void(const core::reload_report&)> done) {
		m_game_reloader->reload_quiet_when_ready(module, std::move(done));
	};
	live.unavailable_reason = [this, has_code] {
		if (std::string reason = m_game_builder->disabled_reason(); !reason.empty()) return reason;
		return has_code ? std::string{} : std::string{ "The project has no code folder (code://)" };
	};
	if (snk_dir) {
		live.store_enabled = [this, dir = *snk_dir](bool enabled) {
			auto problem = update_build_settings(dir,[enabled](build_settings& settings) { settings.live_enabled = enabled; });
			m_build_settings_panel->refresh_if_clean();
			return problem;
		};
	}
	live.on_toggled = [this] {
		if (m_agent_server) m_agent_server->notify_tools_changed();
	};
	live.open_problems = [this] {
		m_context->panels().set_open(m_problems_panel->get_id(), true);
		ImGui::SetWindowFocus(m_problems_panel->get_title().c_str());
	};
	live.open_settings = [this] {
		m_context->panels().set_open(m_build_settings_panel->get_id(), true);
		m_build_settings_panel->focus();
	};
	m_live_mode = std::make_unique<live_mode>(m_context->problems(), m_context->actions(), std::move(live));
	m_live_mode->watch(m_res);
	m_build_settings_panel->set_on_settings_applied([this](const build_settings& settings) { m_live_mode->follow_settings(settings); });
	m_build_settings_panel->set_live_unavailable_reason([this] { return m_live_mode->unavailable_reason(); });
	editor_layer->set_menu_bar_status([this] { m_live_mode->draw_status(); });

	if (snk_dir && load_build_settings(*snk_dir).live_enabled) {
		if (auto refused = m_live_mode->set_enabled(true, false)) {
			egLOG_WARN("editor/build", "live mode is on in .snk/build.json but stays off: {}", *refused);
		}
	}
}

void edt::editor_system::tick_live_mode()
{
	if (m_live_mode) m_live_mode->tick();
}

void edt::editor_system::tick_repository_watcher()
{
	if (!m_repo_watcher || !m_vcs || !m_context) return;
	// Not info(): after a project switch that would run the blocking repository probe here, every frame.
	m_repo_watcher->follow(*m_vcs);
	const edt::vcs::repository_changes changes = m_repo_watcher->tick();
	if (changes.refs) m_context->events().repository_changed.emit(repository_change::refs);
	if (changes.index) m_context->events().repository_changed.emit(repository_change::index);
}

void edt::editor_system::tick_asset_problems()
{
	if (!m_assets || !m_asset_problems || !m_context) {
		return;
	}
	// New files waiting for an ID: a git operation that held them back may have ended (#765).
	m_assets->poll();
	if (m_asset_index) {
		m_asset_index->poll();
	}
	m_asset_problems->publish(m_context->problems(), *m_assets);
}

void edt::editor_system::stop_live_build()
{
	// Not cancel(): that drops the person's request waiting for the live build and gives live mode no outcome.
	if (m_game_builder) m_game_builder->stop_background();
}

void edt::editor_system::create_play_session(ds::app_data_storage& data)
{
	play_session_options options;
	options.input = m_input_router;
	// user:// is the project's test profile; Play gets a fresh one of its own every start.
	if (const auto user_roots = m_res.mount_roots(res::tag::user); !user_roots.empty() && data.has_value<prj::project_context>()) {
		options.profile_resources = &m_res;
		options.play_profile = data.require<prj::project_context>().play_profile_dir();
		options.editor_profile = user_roots;
	}
	if (data.has_value<prj::project_context>()) options.project_root = data.require<prj::project_context>().root;
	options.report_full_snapshot = cfg_hot_reload_dump_snapshot;
	// The problems list, not a dialog: live mode reloads on every build, and a dialog each time would be in the way.
	// Never from inside the swap: the host refuses a revert while a reload runs, so it waits for the next UI frame.
	options.revert_module = [this, &data](const std::string& module) {
		editor_layer->register_implicit("edt/revert_game_module", [this, &data, module] {
			if (!m_play) return false;
			core::project_module_host* host = data.has_value<core::project_module_host>() ? &data.require<core::project_module_host>() : nullptr;
			m_play->run_revert(
				[&] {
					return host ? host->revert(module, m_swap_gate.hooks())
						: core::reload_report{ core::reload_status::refused, "the editor did not load the project's modules" };
				},
				[&] {
					if (host) host->forget_revert(module);
				});
			return false;
		});
	};
	options.notify = [this](const std::string& message) {
		if (message.empty()) {
			m_context->problems().clear("hot_reload");
			return;
		}
		problem item;
		item.severity = problem_severity::warning;
		item.message = message;
		item.notes = { "details: .cache/hot_reload/last_play_snapshot.json" };
		m_context->problems().set("hot_reload", { std::move(item) });
	};
	options.restore_selection = [this](const selection& value) { return m_hierarchy_panel->select_path(value.path, value.target); };
	// The document changes once these land: Play would be built from the old one and the new one land under it.
	options.start_blocker = [this] {
		if (auto migrating = migration_blocker()) return migrating;
		return load_in_progress();
	};
	options.each_frame = [this](const std::string& id, std::function<bool()> poll) { editor_layer->register_implicit(id, std::move(poll)); };
	options.preflight = [this] {
		play_preflight_services services;
		const ecs::system_factory* factory = m_services.sfactory();
		services.systems = factory ? &factory->registry() : nullptr;
		services.descs = m_services.descs();
		services.resources = m_services.resources();
		services.assembler = m_services.assembler();
		auto found = run_play_preflight(m_state, services);
		m_play_preflight.publish(m_context->problems(), found);
		return found;
	};
	options.forget_runtime_entities = [this] {
		if (m_game_view_panel) m_game_view_panel->forget_runtime_entities();
		if (m_viewport_panel) m_viewport_panel->forget_runtime_entities();
	};
	m_play = std::make_unique<play_session>(m_state, m_services, m_world_ctrl, m_context->selection(), m_context->events(), std::move(options));
	if (m_game_view_panel) {
		m_game_view_panel->set_notice([this] {
			return m_play && m_play->reloading() ? std::format("Reloading... ({} resources)", m_play->reloading_resources()) : std::string{};
		});
	}
	m_swap_gate.set_play_hooks([this] { if (m_play) m_play->before_module_swap(); }, [this](const core::reload_report& report) {
		if (m_play) m_play->after_module_swap(report);
		if (m_modes) m_modes->after_module_swap();
	});

	const auto locked = [this] { return m_state.edits_locked(); };
	m_hierarchy_panel->set_edits_locked(locked);
	m_inspector_panel->set_edits_locked(locked);
	m_viewport_panel->set_edits_locked(locked);

	// The Play menu and the agent's play.* (US-63-C, #491). The lambdas check m_play: ~editor_system lets go of it
	// before m_registrations goes.
	for (auto& command : register_play_commands(m_context->actions(),
			{ .play = [this] { return m_play.get(); }, .person_is_editing = [this] { return person_is_editing(); },
				.dialogs = &m_context->dialogs(), .show_problems = [this] {
					m_context->panels().set_open(m_problems_panel->get_id(), true);
					ImGui::SetWindowFocus(m_problems_panel->get_title().c_str());
				} })) {
		m_registrations.push_back(std::move(command));
	}
	// runtime.query / runtime.get (#552): the live entities of Play, read-only.
	for (auto& command : register_runtime_commands(m_context->actions().commands(),
			{ .play = [this] { return m_play.get(); }, .play_set = [this]() -> scn::context_set* {
				const auto manager = m_services.ctx_manager.lock();
				return manager ? manager->find_set(k_play_set) : nullptr;
			} })) {
		m_registrations.push_back(std::move(command));
	}
	// world.events / input.actions (#605): the bus journal and the actions of Play, read-only.
	for (auto& command : register_bus_commands(m_context->actions().commands(),
			{ .play = [this] { return m_play.get(); }, .play_set = [this]() -> scn::context_set* {
				const auto manager = m_services.ctx_manager.lock();
				return manager ? manager->find_set(k_play_set) : nullptr;
			}, .input = [this]() -> const inp::input_router* { return m_input_router; } })) {
		m_registrations.push_back(std::move(command));
	}
	// app.advance / app.resume step the editor's whole frame loop: an agent gets them only during Play.
	if (data.has_value<app::frame_command_registrations>() && data.has_value<app::frame_control>()) {
		play_frame_commands frame_commands = register_play_frame_commands(data.require<app::frame_command_registrations>(),
			m_context->actions().commands(), data.require<app::frame_control>(), [this] { return m_play && m_play->active(); },
			m_context->events());
		m_registrations.push_back(std::move(frame_commands.commands.advance));
		m_registrations.push_back(std::move(frame_commands.commands.resume));
		m_play_hold_release = std::move(frame_commands.release_on_stop);
	}

	// Game mode (#294): F11 / View > Game Mode / editor.mode.toggle. The layer draws the Game view full-window instead
	// of the dockspace; the docked panel keeps its open state, so editor mode is drawn exactly as without it.
	// Headless (set by init before this): no layout, so a Stop leaves the mode alone.
	m_modes = std::make_unique<editor_mode_controller>(*m_play, m_context->events(), !m_context->actions().commands().headless());
	const auto agent_mode_of = [this](std::string_view command_id) {
		// No agent server: no agent can call at all.
		if (!m_agent_access) return cmd::policy_mode::allow;
		const cmd::command_desc* command = m_context->actions().find(command_id);
		return command ? m_agent_access->policy().effective(*command).mode : cmd::policy_mode::deny;
	};
	for (auto& command : register_editor_mode_commands(m_context->actions(), { .modes = [this] { return m_modes.get(); },
			.play = [this] { return m_play.get(); }, .person_is_editing = [this] { return person_is_editing(); },
			.agent_mode_of = agent_mode_of })) {
		m_registrations.push_back(std::move(command));
	}
	editor_layer->set_game_layout([this] { return m_modes && m_modes->mode() == editor_mode::game; },
		[this] {
			m_viewport_panel->suspend_camera();
			m_game_view_panel->render_fullscreen();
		},
		is_game_mode_shortcut,
		[this] { m_game_view_panel->bind_fullscreen_input(); });
	// The start level of --game (#518). Also off once its load's implicit callback is gone, so the screen never outlives it.
	editor_layer->set_loading_screen(
		[this] { return m_modes && m_modes->startup_pending() && editor_layer->has_implicit_prefix("edt/load_level"); },
		[this] { draw_loading_screen(loading_screen_text(m_modes->startup_level_label())); });
	// The Input Actions panel waiting for a key is a modal too: Ctrl+S and the rest must not fire under the player's key (#470).
	editor_layer->set_modal_probe([this] {
		return m_context->dialogs().pending() > 0 || (m_input_actions_panel && m_input_actions_panel->capturing());
	});
	// Bindings two actions share are Problems (source "input"), whether or not the panel is open.
	editor_layer->set_frame_hook([this, &data] {
		// Before the shortcuts of the frame: Ctrl+Z goes to the document of the window the person is in.
		if (m_data_documents) m_data_documents->follow_focus();
		if (!m_context || !data.has_value<gs::input_actions>()) return;
		const auto& actions = data.require<gs::input_actions>();
		if (actions.revision() == m_input_problems_revision) return;
		m_input_problems_revision = actions.revision();
		auto problems = input_actions_view::conflict_problems(actions.conflicts());
		if (problems.empty()) m_context->problems().clear("input");
		else m_context->problems().set("input", std::move(problems));
	});
	m_mode_changed = m_context->events().editor_mode_changed.subscribe([this](editor_mode mode) {
		if (mode == editor_mode::editor) m_viewport_panel->resume_camera();
	});
	m_level_ctrl.on_worlds_replaced = [this] { refresh_world_problems(); };
	m_world_problem_connections.push_back(m_context->events().document_changed.subscribe([this](document_id, std::uint64_t) { refresh_world_problems(); }));
	m_world_problem_connections.push_back(m_context->events().active_world_changed.subscribe([this](document_id) { refresh_world_problems(); }));
	// However the preview goes (another prefab, another kind of preview, a module unloading, quitting).
	m_world_problem_connections.push_back(m_context->events().document_closed.subscribe([this](document_id id) {
		if (id == document_id{} || id != m_desc_prefab_document) return;
		m_desc_prefab_document = document_id{};
		m_desc_problems.clear(m_context->problems(), "prefab");
	}));
	// Popups and dialog_service modals are seen by ImGui; the agent's question is an ordinary window.
	m_game_view_panel->set_input_blocked([this] {
		return m_context->dialogs().pending() > 0 || (m_agent_access && !m_agent_access->confirmations().pending().empty());
	});

	editor_layer->set_menu_bar_status([this] {
		// Left of the others, which place themselves at the right edge. Nothing at all unless an agent can connect or a policy waits.
		draw_agent_status();
		draw_play_status();
		m_live_mode->draw_status();
	});
}

void edt::editor_system::open_migration_window()
{
	if (m_migration_panel == nullptr) return;
	m_context->panels().set_open(m_migration_panel->get_id(), true);
	m_migration_panel->show_and_plan();
}

void edt::editor_system::open_migration_rollback(const std::string& id)
{
	if (m_migration_panel == nullptr) return;
	m_context->panels().set_open(m_migration_panel->get_id(), true);
	m_migration_panel->show_and_roll_back(id);
}

void edt::editor_system::offer_migration_rollback(const journal_offer& offer)
{
	if (offer.what == journal_offer::kind::inform) {
		m_context->dialogs().choose(offer.title, offer.text, { std::string("OK") }, [](std::optional<std::size_t>) {});
		return;
	}
	if (offer.what != journal_offer::kind::ask) return;
	m_context->dialogs().choose(offer.title, offer.text, { std::string(k_journal_offer_roll_back), std::string(k_journal_offer_later) },
		[this, id = offer.id](std::optional<std::size_t> answer) {
			if (answer && *answer == 0) open_migration_rollback(id);
		});
}

void edt::editor_system::start_migration_scan(const std::filesystem::path& root)
{
	const std::filesystem::path settings_file = migration_notice_file(m_res.mount_roots("edt"), default_user_data_dir());
	const std::filesystem::path settings_dir = settings_file.parent_path();
	migration_notice_model::ports ports;
	ports.load = [settings_file]() -> std::optional<std::string> {
		std::error_code ec;
		if (!std::filesystem::is_regular_file(settings_file, ec)) return std::nullopt;
		std::ifstream in(settings_file, std::ios::binary);
		if (!in) return std::nullopt;
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	};
	ports.save = [settings_file](const std::string& text) -> std::optional<std::string> {
		std::error_code ec;
		std::filesystem::create_directories(settings_file.parent_path(), ec);
		return prj::write_text_atomically(settings_file, text);
	};
	m_migration_notice = std::make_unique<migration_notice_model>(std::move(ports));

	// The types are read here, on the main thread; the scan reads only files.
	const migration_notice_model::scan_ticket ticket{ approved_snapshot_for(settings_dir, root).key, m_game_layer_generation, m_migration_runs };
	// Stopped by its token (job.cancel, cancel_all_and_wait when the editor quits) between any two files: a stopped scan is
	// dropped, never shown.
	auto work = [root, catalog = prj::migration::type_catalog::from(*m_services.descs())](ds::task_context& task) {
		prj::migration::scan_options options;
		options.stop = [&task] { return task.stop.stop_requested(); };
		const prj::migration::scan_result scan = prj::migration::scan_project(root, catalog, options);
		if (scan.incomplete) throw ds::task_cancelled{};
		prj::migration::project_step_registry steps;
		prj::migration::register_project_steps(steps);
		migration_scan_summary summary = summarize_scan(scan, steps);
		summary.unfinished = prj::migration::unfinished_migrations(root);
		return summary;
	};
	m_migration_scan_job = m_context->jobs().start(ds::lane::io, ds::task_desc{ "Check the project's format", "project", false }, m_scan_lifetime,
		std::move(work), [this, ticket](ds::task_result<migration_scan_summary>& result) {
			if (!result.ok() || result.value == nullptr || m_migration_notice == nullptr) return;
			// The editor never changes its project; the game layer may have (a module loaded or reloaded meanwhile), and a
			// migration or rollback that started since changes the files the scan read.
			const migration_notice_model::scan_ticket now{ ticket.project, m_game_layer_generation, m_migration_runs };
			if (m_migration_notice->deliver(ticket, now, std::move(*result.value)) && m_migration_notice->visible()) {
				egLOG_INFO("project/migrate", "{} Project > Migrate Project... shows the plan.", m_migration_notice->text());
			}
		});
	editor_layer->register_implicit("edt/migration_notice", [this] { return draw_migration_notice(); });
}

bool edt::editor_system::draw_migration_notice()
{
	if (m_migration_notice == nullptr) return false;
	if (m_migration_scan_job != 0 && m_context->jobs().is_running(m_migration_scan_job)) return true;
	if (m_migration_running) m_migration_notice->hide();
	if (!m_migration_notice->visible()) return false;
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	// Above the agent policy's notice, which takes the corner.
	ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12.f, viewport->WorkPos.y + viewport->WorkSize.y - 110.f),
		ImGuiCond_Always, ImVec2(1.f, 1.f));
	const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
		| ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse;
	if (ImGui::Begin("Project format##migration_notice", nullptr, flags)) {
		const std::string text = m_migration_notice->text();
		ImGui::TextUnformatted(text.c_str());
		if (ImGui::Button("Open Migration...")) {
			m_migration_notice->open();
			open_migration_window();
		}
		ImGui::SameLine();
		if (ImGui::Button("Later")) static_cast<void>(m_migration_notice->later());
	}
	ImGui::End();
	return true;
}

void edt::editor_system::open_agents_panel()
{
	if (m_agents_panel == nullptr) return;
	m_context->panels().set_open(m_agents_panel->get_id(), true);
	m_agents_panel->focus();
}

void edt::editor_system::draw_agent_policy_notice()
{
	if (m_agent_access == nullptr) return;
	if (!m_agent_access->policy().project_pending_approval()) {
		m_policy_notice_dismissed_key.clear();
		return;
	}
	// agent_access::frame reads the files every frame while the server is active; a waiting policy must also notice, about once
	// a second, that its file was fixed or changed again. Nothing is done while no policy waits (the idle editor).
	if (m_agent_server == nullptr || !m_agent_server->active()) {
		const auto now = std::chrono::steady_clock::now();
		if (now - m_policy_refreshed >= std::chrono::seconds(1)) {
			m_policy_refreshed = now;
			m_agent_access->policy().refresh();
			if (!m_agent_access->policy().project_pending_approval()) return;
		}
	}
	std::string pending_key;
	for (const policy_loosening& change : m_agent_access->policy().pending_loosenings()) {
		pending_key += change.what + '|' + cmd::to_string(change.from) + '|' + cmd::to_string(change.to) + '\n';
	}
	if (m_policy_notice_dismissed_key == pending_key) return;
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12.f, viewport->WorkPos.y + viewport->WorkSize.y - 12.f),
		ImGuiCond_Always, ImVec2(1.f, 1.f));
	const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
		| ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse;
	if (ImGui::Begin("Agent policy##agent_policy_notice", nullptr, flags)) {
		ImGui::TextUnformatted("The project's agent policy waits for your approval.");
		if (ImGui::Button("Open")) {
			open_agents_panel();
			m_policy_notice_dismissed_key = pending_key;
		}
		ImGui::SameLine();
		if (ImGui::Button("Later")) m_policy_notice_dismissed_key = pending_key;
	}
	ImGui::End();
}

void edt::editor_system::draw_agent_status()
{
	if (m_agent_access == nullptr || m_agent_server == nullptr) return;
	const bool pending = m_agent_access->policy().project_pending_approval();
	// Before any ImGui call: for a person who does not use agents the menu bar is what it was.
	if (!pending && !m_agent_server->active()) return;

	const std::vector<agent_session_info> sessions = m_agent_server->sessions();
	const std::size_t connected = sessions.size() + (m_agent_server->stdio_session().empty() ? 0u : 1u);
	const std::size_t questions = m_agent_access->confirmations().pending().size();
	const bool flash = !m_recent_agent_actions.empty()
		&& std::chrono::steady_clock::now() - m_recent_agent_actions.back().at < std::chrono::seconds(2);

	std::string label = pending ? std::string("Agent policy: approve")
		: questions > 0 ? std::format("Agent: {} waiting", questions) : std::format("Agent: {}", connected);
	label += "###agent_status";
	const ImVec4 color = pending || questions > 0 ? ImVec4(0.95f, 0.75f, 0.3f, 1.0f)
		: flash ? ImVec4(0.4f, 0.85f, 0.4f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
	const float width = ImGui::CalcTextSize(label.c_str(), nullptr, true).x;
	ImGui::PushStyleColor(ImGuiCol_Text, color);
	const bool clicked = ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(width, 0.f));
	ImGui::PopStyleColor();
	if (clicked) ImGui::OpenPopup("agent_status_popup");
	if (ImGui::BeginPopup("agent_status_popup")) {
		if (pending) ImGui::TextUnformatted("The project's agent policy waits for your approval.");
		if (m_recent_agent_actions.empty()) {
			ImGui::TextDisabled("No agent action yet.");
		}
		for (auto it = m_recent_agent_actions.rbegin(); it != m_recent_agent_actions.rend(); ++it) {
			// Paths and ids of ours, but shown as plain text all the same.
			const std::string line = it->command + "  " + it->target;
			ImGui::TextUnformatted(line.c_str());
		}
		ImGui::Separator();
		for (const agent_session_info& session : sessions) {
			ImGui::PushID(session.id.c_str());
			ImGui::TextUnformatted(session.client_name.empty() ? "(unnamed)" : session.client_name.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("Disconnect")) m_agent_server->close_session(session.id);
			ImGui::PopID();
		}
		if (ImGui::MenuItem("Settings...")) open_agents_panel();
		ImGui::EndPopup();
	}
}

void edt::editor_system::draw_play_status()
{
	// Stopped: nothing at all, so the menu bar is what it is without Play.
	if (!m_play || !m_play->active()) return;
	const bool paused = m_play->state() == play_state::paused;
	ImGui::TextColored(paused ? ImVec4(0.95f, 0.75f, 0.3f, 1.0f) : ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "%s",
		paused ? "Play: paused" : "Play: playing");
}

void edt::editor_system::create_panels(bool capture_log)
{
	m_hierarchy_panel = std::make_shared<scene_hierarchy_panel>();
	m_inspector_panel = std::make_shared<inspector_panel>();
	m_inspector_panel->set_inspector_registry(&m_context->inspectors());
	m_console_panel = std::make_shared<console_panel>();
	m_asset_browser_panel = std::make_shared<asset_browser_panel>();
	m_asset_browser_panel->set_layers(m_res.get_resource_layers());
	if (cfg_ui_probe)
		m_asset_browser_panel->set_ui_probe(std::make_shared<ui_probe>());
	if (const auto code_roots = m_res.mount_roots(prj::k_code_protocol); !code_roots.empty()) {
		code_actions actions{ m_res, code_tree_model{ code_roots.back() },
			[](const std::filesystem::path& file) { return platform::open_in_shell(file); } };
		m_asset_browser_panel->set_code_section(std::make_shared<code_section>(m_res, std::move(actions)));
	}

	if (!capture_log) return;

	auto console_weak = std::weak_ptr<console_panel>(m_console_panel);
	m_log_sink = engine::add_log_sink([console_weak](std::string_view category, engine::log_level level, std::string_view message) {
		auto panel = console_weak.lock();
		if (!panel) return;
		edt::log_level edt_level = edt::log_level::info;
		if (level == engine::log_level::warn)  edt_level = edt::log_level::warning;
		if (level == engine::log_level::error) edt_level = edt::log_level::error;
		panel->add_log(edt_level, std::format("[{}] {}", category, message));
	});
}

void edt::editor_system::create_viewport_panel()
{
	m_viewport_panel = std::make_shared<viewport_panel>(m_rnd, m_gui);
	m_viewport_panel->set_registry_getter([this]() -> entt::registry* { return view_registry(m_services, m_state); });
	m_viewport_panel->set_input_router(m_input_router, [this] { return active_input_target(m_services, m_state); });
	m_viewport_panel->set_on_asset_dropped([this](const std::string& res_rel_path) {
		if (!m_state.editor_tag.is_valid()) return;
		m_context->asset_kinds().drop_into_scene(*m_context, res::tag::make(res_rel_path));
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
			m_desc_problems.report(m_context->problems(), "level", diagnostics_of(handle), m_state.editor_tag, m_state.level_tag,
				level_locator(handle, m_state.editor_tag, m_state.level_tag));
		});
		ctx_mgr->set_on_game_layer_change([this](scn::context_manager::game_layer_change change) {
			++m_game_layer_generation;
			m_swap_gate.on_game_layer_change(change == scn::context_manager::game_layer_change::before);
		});
		// The set "level" is the edited document: a game's request_level only ever replaces the worlds of Play (#564).
		ctx_mgr->set_level_set_switchable(false);
		ctx_mgr->set_on_level_switched([this](scn::context_set& set) {
			if (set.name() == k_play_set && m_play) m_play->on_level_switched();
		});
	}
}

void edt::editor_system::follow_game_layer_change(bool before)
{
	if (before) {
		// Nothing of the editor may refer to the old layer once it goes: the set "play", the preview set, the loads in
		// flight, the level. A reload of the module stopped Play already (module_swap_gate) and starts it again.
		// A plain module load or unload (no swap): the person's Play ends, nothing restarts it.
		if (m_play && m_play->active()) m_play->stop(play_reason::user);
		close_prefab_editor();
		m_pending_desc_handle = {};
		m_level_after_layer_change = res::tag{};
		if (m_loading_level) {
			m_level_after_layer_change = m_loading_level_tag;
			abandon_level_load();
		}
		m_level_ctrl.detach_from_game_layer();
		return;
	}
	if (scn::context_set* level = m_services.level_set()) {
		level->set_mode(ecs::run_mode::edit);
	}
	m_level_ctrl.attach_to_game_layer();
	if (m_level_after_layer_change.is_valid()) {
		begin_level_load(std::exchange(m_level_after_layer_change, res::tag{}));
	}
}

void edt::editor_system::wire_panel_callbacks()
{
	m_hierarchy_panel->set_on_world_changed([this](int idx) {
		// A refused switch leaves the old world active: resync the tab and the panels to it.
		if (!m_world_ctrl.switch_to_world(idx) && m_world_ctrl.on_world_switched)
			m_world_ctrl.on_world_switched(m_state.active_world_idx);
	});
	m_hierarchy_panel->set_on_create_world([this](const std::string& name) {
		m_world_ctrl.create_world(name);
	});
	m_hierarchy_panel->set_on_create_node([this](const std::string& type_name) {
		m_scene_editor.create_entity(type_name);
	});
	m_hierarchy_panel->set_on_delete_node([this](uint64_t node_id) {
		m_scene_editor.delete_entity(node_id);
	});
	m_hierarchy_panel->set_on_delete_nodes([this](const std::vector<uint64_t>& node_ids) {
		m_scene_editor.delete_entities(node_ids);
	});
	m_hierarchy_panel->set_on_rename_node([this](const std::string& /*old_name*/, const std::string& /*new_name*/) {
		m_state.commit("Rename node");
	});
	m_hierarchy_panel->set_on_duplicate_node([this](uint64_t node_id) {
		m_scene_editor.duplicate_entity(node_id);
	});
	m_hierarchy_panel->set_on_duplicate_nodes([this](const std::vector<uint64_t>& node_ids) {
		m_scene_editor.duplicate_entities(node_ids);
	});
	m_hierarchy_panel->set_on_reparent_node([this](uint64_t node_id, uint64_t new_parent_id, int insert_index) {
		m_scene_editor.reparent_entity(node_id, new_parent_id, insert_index);
	});

	m_inspector_panel->set_on_node_changed([this]() {
		m_state.commit("Edit node");
	});

	m_viewport_panel->set_on_transform_committed(
		[this](std::span<const uint64_t> path, glm::vec3 pos, glm::vec3 rot, glm::vec3 scale) {
			m_scene_editor.on_transform_committed(path, pos, rot, scale);
		});
}

// ─── Import pipeline ────────────────────────────────────────────────────────

bool edt::editor_system::poll_import_pipeline()
{
	if (m_export_dialog_active) {
		bool still_open = m_export_dialog->render();
		if (!still_open) {
			m_export_dialog_active = false;
			auto exported_tag = m_export_dialog->get_exported_tag();
			if (exported_tag.is_valid() && m_state.editor_tag.is_valid()
				&& !refused_during_play(m_play.get(), m_context->dialogs(), "The asset is exported; stop Play to add it to the scene.")) {
				std::string name_str = std::string(exported_tag.pure_name());
				m_pending_desc_handle = m_services.resources()->require<desc::desc_base>(exported_tag);
				m_pending_desc_tag = exported_tag;
				m_pending_desc_key = m_scene_editor.make_unique_key(name_str.empty() ? "ImportedModel" : name_str);
				editor_layer->register_implicit("edt/export_add", [this] { return poll_pending_desc(); });
			}
			return false;
		}
		return true;
	}

#if SNK_HAS_ASSIMP
	if (m_importing) {
		return poll_import_progress();
	}
#endif
	return false;
}

#if SNK_HAS_ASSIMP
void edt::editor_system::start_import(const std::filesystem::path& abs_path)
{
	if (m_importing) return;
	if (m_agent_import && m_agent_import->running()) {
		m_context->dialogs().message(message_level::info, "An import asked for by an agent is running: wait for it, or cancel it in the job list.");
		return;
	}
	if (refused_during_play(m_play.get(), m_context->dialogs(), "Stop Play to import.")) return;

	m_importing = true;
	m_import_finished = false;
	m_import_spinner_t = 0.f;
	m_import_result.reset();
	m_import_failure.clear();
	m_import_name = abs_path.filename().string();
	// compute lane: assimp parsing and mesh building are CPU-bound; the io lane has two threads for blocking files.
	m_import_job = m_context->jobs().start(ds::lane::compute,
		{ "Import " + m_import_name, "import" }, m_lifetime,
		[importer = m_model_importer.get(), abs_path](ds::task_context& ctx) -> edt::import_intermediate {
			ctx.throw_if_stopped();
			return importer->import_background(abs_path, &ctx);
		},
		[this](ds::task_result<edt::import_intermediate>& result) {
			if (!m_importing) return;
			m_import_finished = true;
			if (result.ok()) {
				m_import_result = std::move(*result.value);
			} else {
				m_import_failure = result.error;
			}
		});
	editor_layer->register_implicit("edt/import_model", [this] { return poll_import_pipeline(); });
}

void edt::editor_system::finish_import()
{
	m_importing = false;
	m_import_finished = false;
	m_import_job = 0;
	m_import_result.reset();
	m_import_failure.clear();
	m_import_name.clear();
}

bool edt::editor_system::poll_import_progress()
{
	// Not running and not finished: the job was cancelled and its result discarded.
	const bool waiting = !m_import_finished && m_context->jobs().is_running(m_import_job);

	if (waiting) {
		ImGui::OpenPopup("Importing##edt_import");
		if (ImGui::BeginPopupModal("Importing##edt_import", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
		{
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
				ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				m_context->jobs().cancel(m_import_job);
				finish_import();
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return false;
			}
			m_import_spinner_t += ImGui::GetIO().DeltaTime;
			const char* spinners[] = { "|", "/", "-", "\\" };
			int idx = static_cast<int>(m_import_spinner_t * 8.f) % 4;
			ImGui::Text("%s  Importing '%s'...", spinners[idx], m_import_name.c_str());
			ImGui::EndPopup();
		}
		return true;
	}

	ImGui::OpenPopup("Importing##edt_import");
	if (ImGui::BeginPopupModal("Importing##edt_import", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (!m_import_result) {
		if (!m_import_failure.empty()) {
			egLOG("Editor/Import", "Async import failed: {}", m_import_failure);
		}
		finish_import();
		return false;
	}

	auto intermediate = std::move(*m_import_result);
	finish_import();

	if (!intermediate.error.empty()) {
		egLOG("Editor/Import", "Async import failed: {}", intermediate.error);
		m_context->dialogs().message(message_level::error, std::format("Import of '{}' failed: {}", intermediate.source_path.filename().string(), intermediate.error));
		return false;
	}

	auto result = m_model_importer->finalize_on_main(std::move(intermediate));
	if (result.prefab) {
		egLOG("Editor/Import", "Model imported to memory: {}", result.root_tag.view());
		m_export_dialog->open(std::move(result));
		m_export_dialog_active = true;
		return true;
	}

	egLOG("Editor/Import", "Failed to finalize import");
	return false;
}
#endif

// ─── Level load ─────────────────────────────────────────────────────────────

void edt::editor_system::run_implicit_callbacks()
{
	if (editor_layer) editor_layer->process_implicit();
}

bool edt::editor_system::begin_level_load(const res::tag& original_tag)
{
	// Through the resolver: a level in any res:// layer (a lower content root, the engine) is found.
	auto& game_resources = *m_services.resources();
	std::optional<std::vector<std::byte>> level_bytes;
	if (const auto raw = game_resources.fetch_data(original_tag)) {
		raw->wait();
		if (raw->is_ready()) level_bytes = raw->data;
	}
	if (level_bytes && names_another_desc_type(*level_bytes)) {
		egLOG_ERROR("Editor/Load", "'{}' is not a level file (its __type is not '{}')", original_tag.view(), scn::level_desc::__type);
		return false;
	}

	// Another level replaces the one Play was built from.
	if (m_play && m_play->active()) m_play->stop();
	m_level_ctrl.unwatch_level();

	std::string stem = std::filesystem::path(std::string(original_tag.relative())).stem().string();
	// A load that was never finished (begin_level_load called again): its copy goes.
	if (m_loading_level) abandon_level_load();
	cancel_level_settle();
	m_loading_editor_tag = res::tag{ "memory://editor/loading/" + std::to_string(++m_load_serial) + "/" + stem + ".desc" };
	m_loading_level_tag = original_tag;

	if (level_bytes) m_services.writable_resources()->store(m_loading_editor_tag, *level_bytes);

	m_loading_level_handle = game_resources.require<scn::level_desc>(m_loading_editor_tag);
	m_loading_level = true;
	m_slow_load.start(slow_load_notice::clock::now());
	return true;
}

void edt::editor_system::release_level_load()
{
	m_loading_editor_tag = res::tag{};
	m_loading_level_tag = res::tag{};
	m_loading_level = false;
	m_loading_level_handle = {};
}

void edt::editor_system::erase_loading_copy(const res::tag& copy)
{
	constexpr std::string_view prefix = "memory://editor/loading/";
	if (!copy.is_valid() || !copy.view().starts_with(prefix)) return;
	m_services.writable_resources()->erase(copy);
}

void edt::editor_system::abandon_level_load()
{
	const res::tag copy = m_loading_editor_tag;
	release_level_load();
	erase_loading_copy(copy);
}

cmd::result edt::editor_system::cancel_level_load(const cmd::origin& from)
{
	// A load that finished or failed is no longer one: nothing to undo, and the level stays as that load left it.
	if (!m_loading_level) return cmd::result::failure(cmd::codes::failed, "no level is loading");
	const res::tag level = m_loading_level_tag;
	abandon_level_load();
	m_level_ctrl.resume_watch();
	const char* who = from.kind == cmd::origin_kind::agent ? "an agent" : from.kind == cmd::origin_kind::ui ? "a person"
		: from.kind == cmd::origin_kind::test ? "a test" : "the game";
	egLOG_INFO("editor", "the load of level '{}' was cancelled by {}{}", level.view(), who,
		from.session.empty() ? std::string{} : " (session " + from.session + ")");
	boost::json::object out;
	out["cancelled"] = true;
	out["level"] = boost::json::string_view(level.view().data(), level.view().size());
	return cmd::result::success(std::move(out));
}

bool edt::editor_system::poll_level_load(bool* loaded_out)
{
	// The load was refused (not a level) or is gone: nothing to wait for.
	if (!m_loading_level) {
		if (loaded_out) *loaded_out = false;
		return false;
	}
	if (m_loading_level_handle.has_error()) {
		scn::report_level_failed(m_loading_level_tag, m_loading_level_handle);
		m_desc_problems.report(m_context->problems(), "level", diagnostics_of(m_loading_level_handle), m_loading_editor_tag,
			m_loading_level_tag);
		if (loaded_out) *loaded_out = false;
		abandon_level_load();
		m_level_ctrl.resume_watch();
		return false;
	}

	if (!m_loading_level_handle.is_ready()) {
		if (const auto waited = m_slow_load.due(slow_load_notice::clock::now())) {
			egLOG_WARN("editor", "level '{}' is still loading after {} s; level.cancel_load (File > Cancel Level Load) cancels it",
				m_loading_level_tag.view(), waited->count());
		}
		ImGui::OpenPopup("Loading Level##edt_load");
		if (ImGui::BeginPopupModal("Loading Level##edt_load", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
		{
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
				ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				if (loaded_out) *loaded_out = false;
				static_cast<void>(cancel_level_load(cmd::origin::ui()));
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return false;
			}
			ImGui::Text("Loading level...");
			ImGui::EndPopup();
		}
		return true;
	}

	ImGui::OpenPopup("Loading Level##edt_load");
	if (ImGui::BeginPopupModal("Loading Level##edt_load", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// Play cannot start during a load (start_blocker); one that runs anyway is of the level being replaced.
	if (m_play && m_play->active()) m_play->stop();
	const res::tag editor_tag_before = m_state.editor_tag;
	const bool loaded = m_level_ctrl.take_loaded_level(m_loading_level_handle, m_loading_level_tag, m_loading_editor_tag);
	if (loaded_out) *loaded_out = loaded;
	// Warnings of a level that loaded too (an unknown component); none: the scope goes.
	m_desc_problems.report(m_context->problems(), "level", diagnostics_of(m_loading_level_handle), m_loading_editor_tag,
		m_loading_level_tag, level_locator(m_loading_level_handle, m_loading_editor_tag, m_loading_level_tag));
	if (loaded) {
		// The previous level's working copy has no user left: the level set was rebuilt from the new one.
		release_level_load();
		if (editor_tag_before != m_state.editor_tag) erase_loading_copy(editor_tag_before);
	} else {
		abandon_level_load();
	}
	if (loaded) {
		refresh_world_problems();
		start_level_settle(m_state.level_tag);
	}
	return false;
}

void edt::editor_system::start_level_settle(const res::tag& level)
{
	m_settle_contexts = m_services.ctx_manager.lock();
	ASSERT_MSG(m_settle_contexts && m_tasks, "a level loaded before editor_system::init");
	m_level_settle.emplace(*m_settle_contexts, m_res, *m_tasks);
	m_level_settle->start(level);
}

void edt::editor_system::cancel_level_settle()
{
	m_level_settle.reset();
	m_settle_contexts.reset();
}

void edt::editor_system::tick_level_settle()
{
	if (!m_level_settle || !m_level_settle->poll()) return;
	cancel_level_settle();
	if (m_stop_after_level_loaded) {
		egLOG_INFO("app", "exit after the level loaded (editor/exit_after_level_loaded)");
		m_stop_after_level_loaded();
	}
}

void edt::editor_system::refresh_world_problems()
{
	if (!m_context) return;
	m_world_problems.refresh(m_context->problems(), m_state, m_services.level_set());
}

void edt::editor_system::go_to_problem(const std::string& source, const problem& item)
{
	if (source == desc_problem_publisher::source) {
		if (const auto target = m_desc_problems.target_of(item)) go_to(*target);
		return;
	}
	if (source == play_preflight_publisher::source) {
		if (const auto target = m_play_preflight.target_of(item)) go_to(*target);
		return;
	}
	if (source == asset_problem_publisher::source) {
		const std::optional<std::string> file = m_asset_problems ? m_asset_problems->file_of(item) : std::nullopt;
		if (!file) {
			return;
		}
		desc_target target;
		target.resource = *file;
		go_to(target);
		return;
	}
	if (source == world_problem_publisher::source) {
		const auto world = m_world_problems.world_of(item);
		if (!world) return;
		desc_target target;
		target.level = m_state.level_tag.string();
		target.world = *world;
		go_to(target);
	}
}

void edt::editor_system::go_to(const desc_target& target)
{
	if (!target.world.empty()) {
		if (target.level != m_state.level_tag.string()) {
			egLOG_INFO("editor", "open the level '{}' to go to the node", target.level);
			return;
		}
		if (m_play && m_play->active()) {
			egLOG_INFO("editor", "stop Play to go to the node");
			return;
		}
		const auto found = std::ranges::find(m_state.world_names, target.world);
		const auto index = static_cast<std::size_t>(found - m_state.world_names.begin());
		if (found == m_state.world_names.end() || index >= m_state.world_descs.size()) {
			egLOG_INFO("editor", "the world '{}' is no longer in the level", target.world);
			return;
		}
		if (!target.path.empty()) {
			const auto address = scene_editor::resolve_node_path(m_state.world_descs[index].get_root().children, target.path);
			if (!address.owner || !address.target.empty()) {
				egLOG_INFO("editor", "the node of this problem is no longer in the world '{}'", target.world);
				return;
			}
		}
		if (m_state.active_world_idx != static_cast<int>(index)) {
			m_world_ctrl.select_world(static_cast<int>(index));
			// A world the level set does not build is refused: the selection must not point into an inactive world.
			if (m_state.active_world_idx != static_cast<int>(index)) {
				egLOG_INFO("editor", "the world '{}' could not be made active", target.world);
				return;
			}
		}
		if (target.path.empty()) return;
		selection value;
		value.document = m_state.id();
		value.world = target.world;
		value.path = target.path;
		m_context->selection().select(std::move(value));
		return;
	}
	if (target.resource.empty()) return;
	if (!open_asset(res::tag{ target.resource }, true)) {
		egLOG_INFO("editor", "nothing in the editor opens '{}'", target.resource);
	}
}

std::optional<std::string> edt::editor_system::migration_blocker() const
{
	if (m_migration_running) return std::string{ "a project migration is running" };
	return std::nullopt;
}

std::optional<std::string> edt::editor_system::project_write_blocker()
{
	if (auto migrating = migration_blocker()) return migrating;
	return m_migration_stopped.reason();
}

std::optional<std::string> edt::editor_system::load_in_progress() const
{
	if (m_loading_level) return std::string{ "a level is loading" };
	if (m_pending_desc_handle.is_valid()) return std::string{ "an asset is being added to the scene" };
	return std::nullopt;
}

bool edt::editor_system::poll_pending_desc()
{
	// Dropped when the game layer changed under the load.
	if (!m_pending_desc_handle.is_valid()) return false;
	if (m_pending_desc_handle.has_error()) {
		egLOG_ERROR("Editor/Drop", "Failed to load desc '{}'", m_pending_desc_tag.view());
		m_desc_problems.report(m_context->problems(), "drop", diagnostics_of(m_pending_desc_handle));
		m_pending_desc_handle = {};
		return false;
	}

	if (!m_pending_desc_handle.is_ready()) {
		ImGui::OpenPopup("Loading Asset##edt_drop");
		if (ImGui::BeginPopupModal("Loading Asset##edt_drop", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
		{
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
				ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				m_pending_desc_handle = {};
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return false;
			}
			ImGui::Text("Loading '%s'...", m_pending_desc_key.c_str());
			ImGui::EndPopup();
		}
		return true;
	}

	ImGui::OpenPopup("Loading Asset##edt_drop");
	if (ImGui::BeginPopupModal("Loading Asset##edt_drop", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	m_desc_problems.report(m_context->problems(), "drop", diagnostics_of(m_pending_desc_handle));
	m_level_ctrl.add_desc_to_scene(m_pending_desc_handle, m_pending_desc_key);
	m_pending_desc_handle = {};
	return false;
}

// ─── Prefab editor (EPIC-25) ────────────────────────────────────────────────

void edt::editor_system::open_prefab_for_edit(const res::tag& tag)
{
	prefab_document& prefab = edt::open_prefab(m_context->documents(), tag);
	// The diagnostics of the load the preview made, or of a load again when the preview let it go meanwhile.
	const auto loaded = m_services.resources()->require_sync<scn::prefab_desc>(tag);
	m_desc_problems.report(m_context->problems(), "prefab", diagnostics_of(loaded));
	m_desc_prefab_document = prefab.is_open() ? prefab.id() : document_id{};
	if (prefab.is_open()) {
		m_prefab_panel->set_context(&prefab);
		m_prefab_panel->set_open(true);
	}
}

edt::desc_file_layout edt::editor_system::desc_layout()
{
	desc_file_layout layout;
	layout.top_layer = [this] { return m_res.get_resources_path(); };
	layout.layers = [this] { return m_res.get_resource_layers(); };
	layout.open_level_files = [this] {
		std::vector<std::string> open;
		for (const res::tag* tag : { &current_level_tag(), &current_editor_tag() }) {
			if (!tag->view().empty()) open.emplace_back(tag->relative());
		}
		return open;
	};
	layout.write_blocker = [this] { return project_write_blocker(); };
	return layout;
}

// ─── Data documents (EPIC-56, #793) ─────────────────────────────────────────

bool edt::editor_system::open_asset(const res::tag& tag, bool quiet)
{
	if (m_context->asset_kinds().open(*m_context, tag)) return true;
	if (!tag.relative().ends_with(".desc")) return false;
	m_data_documents->open(tag, quiet);
	return true;
}

bool edt::editor_system::person_is_editing() const
{
	// A text field that only has the keyboard is no gesture; a typed value not yet committed is the inspector's pending commit.
	return (ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput) || (m_inspector_panel && m_inspector_panel->has_pending_commit())
		|| (m_viewport_panel && m_viewport_panel->is_dragging_gizmo());
}

void edt::editor_system::close_prefab_editor()
{
	auto& documents = m_context->documents();
	if (prefab_document* prefab = prefab_of(documents)) {
		documents.close(prefab->id());
	}
	m_prefab_panel->set_context(nullptr);
	m_prefab_panel->set_open(false);
}
