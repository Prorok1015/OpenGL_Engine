#include "edt_editor_system.h"
#include "edt_begin_then_poll.h"
#include <snk/editor/edt_editor_context.h>
#include <snk/common/engine_assert.h>
#include "edt_selection_wiring.h"
#include "edt_prefab_document.h"
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
#include "edt_asset_browser_panel.h"
#include "edt_prefab_editor_panel.h"
#include "edt_component_renderers.h"
#include "edt_builtin_asset_kinds.h"
#include "edt_model_importer.h"
#include "edt_asset_exporter.h"
#include "edt_asset_export_dialog.h"
#include "edt_async_import_task.h"
#include "edt_input_target.h"
#include "edt_file_menu.h"
#include "edt_game_builder.h"
#include "edt_main_menu.h"
#include "edt_agent_server.h"
#include "edt_engine_version_prompt.h"
#include "edt_recent_projects.h"
#include <snk/application/app_deterministic_time.h>
#include <snk/command/cmd_registry.h>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include <snk/project/prj_create.h>
#include <snk/project/prj_project.h>
#include <snk/platform/plt_process.h>
#include <snk/editor/edt_dialog_service.h>
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
#include <entt/entt.hpp>
#include <boost/json/object.hpp>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Editor/data (editor_module): the project templates live under it.
CFG_VAR_EXT_PATH(cfg_edt_data_path);

namespace {
	std::string utf8_of(const std::filesystem::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

	// Switching projects restarts the editor (EPIC-42 #158): the same program and config file, the other project.
	// Overrides given with --set are not carried over.
	std::optional<std::string> start_editor_on(const std::filesystem::path& manifest_file, bool allow_engine_version_mismatch)
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
	// Opens the Game view at startup (automation); otherwise it is toggled from the View menu.
	CFG_VAR_DEF_BOOL(cfg_show_game_view, "editor/show_game_view", false);
	// The MCP server for AI agents (read-only: log, jobs, dialogs, screenshots). Off unless the user turns it on.
	CFG_VAR_DEF_BOOL(cfg_agent_server, "editor/agent_server", false);
	// 0: the operating system picks a free port; it is written to <project dir>/.snk/mcp.json with the token.
	CFG_VAR_DEF_INT(cfg_agent_server_port, "editor/agent_server_port", 0);
	// Where .snk/mcp.json goes; empty: the open project's root, or the working directory without a project.
	CFG_VAR_DEF_STRING(cfg_project_dir, "editor/project_dir", "");
	// Build > Build Game (#434). Empty paths: found by the editor (see game_build_sdk_dir, edt_toolchain.h).
	CFG_VAR_DEF_PATH(cfg_build_sdk_dir, "build/sdk_dir", "");
	CFG_VAR_DEF_PATH(cfg_build_cmake, "build/cmake", "");
	CFG_VAR_DEF_PATH(cfg_build_vs_path, "build/vs_path", "");
	CFG_VAR_DEF_STRING(cfg_build_vs_min_version, "build/vs_min_version", "17.14");
	CFG_VAR_DEF_STRING(cfg_build_configuration, "build/configuration", "debuggame");

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
	// The context (and its document_manager) outlives this system, which owns the scene document: let go of it first.
	// Gone already (storage torn down without editor_term): its destructor stopped the jobs and let go of the document.
	if (const std::shared_ptr<edt::editor_context> context = m_context_alive.lock()) {
		// Jobs run the importer and exporter of this system: none may be left running when they are destroyed.
		context->jobs().cancel_all_and_wait();
		context->documents().shutdown();
	}
	if (m_prefab_panel) {
		m_prefab_panel->set_context(nullptr);
	}
	if (auto ctx_mgr = m_services.ctx_manager.lock()) {
		ctx_mgr->set_on_game_layer_change(nullptr);
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
	// The editor scene is authored, not played: only run_in_edit systems tick (transforms, camera, animation preview).
	m_services.ctx_manager.lock()->level_set().set_mode(ecs::run_mode::edit);
	m_services.base_descs     = &desc_system;
	m_services.base_resources = &m_res;
	m_services.base_sfactory  = &data.require<ecs::system_factory>();
	m_services.base_assembler = &data.require<scn::ecs_assembler>();
	ASSERT_MSG(!data.has_value<edt::editor_context>(), "editor_system::init called twice: editor_context is already registered");
	m_context = &data.construct<edt::editor_context>(m_res, m_rnd, m_gui, m_services,
		data.require<ds::task_system>(), data.require<cmd::command_registry>(), m_state);
	m_context_alive = data.require_shared<edt::editor_context>();
	editor_layer->set_context(*m_context);
	m_export_dialog = std::make_unique<edt::asset_export_dialog>(*m_asset_exporter, m_context->jobs(), m_res);
	m_context->dialogs().set_registrar([this](std::string_view id, std::function<bool()> draw) {
		editor_layer->register_implicit(id, std::move(draw));
	});
	// The panels own input routing: whichever scene image is hovered feeds its context, nothing else does.
	m_input_router = &data.require<inp::input_router>();
	m_input_router->set_default_target(std::nullopt);

	// Create panels
	// Load order, absolute paths and timings in the log text differ between runs and would show in screenshots.
	create_panels(!(data.has_value<app::deterministic_time>() && data.require<app::deterministic_time>().enabled()));
	for (auto& renderer : register_desc_component_renderers(m_context->inspectors())) m_registrations.push_back(std::move(renderer));
	m_inspector_panel->set_desc_source([this] { return m_services.descs(); });
	{
		// One composed view of the instances for the inspector and the hierarchy; edits and pushes invalidate it.
		auto view_cache = std::make_shared<override_edit::view_cache>(m_services, m_state);
		m_inspector_panel->set_view_cache(view_cache);
		m_hierarchy_panel->set_view_cache(std::move(view_cache));
	}
	create_viewport_panel();

	// Wire all controller + panel callbacks
	wire_controller_callbacks();
	wire_panel_callbacks();
	m_context->selection().set_registry_provider([this](const std::string& world) -> entt::registry* {
		scn::context_set* level = m_services.level_set();
		scn::world_context* found = level ? level->find(world) : nullptr;
		return found ? &found->state() : nullptr;
	});
	m_selection_connections = wire_selection(
		m_context->events(), m_context->selection(), m_state, m_scene_editor, m_world_ctrl,
		*m_hierarchy_panel, *m_inspector_panel,
		[this](ecs::entity ent) { m_viewport_panel->set_selected_entity(ent); });

	// Create prefab editor panel (EPIC-25)
	m_prefab_panel = std::make_shared<prefab_editor_panel>(m_rnd, m_gui);
	m_prefab_panel->set_on_close([this] { close_prefab_editor(); });
	m_prefab_panel->set_input_router(m_input_router);

	asset_kind_hooks asset_hooks;
	asset_hooks.open_prefab = [this](const res::tag& tag) { open_prefab_for_edit(tag); };
	asset_hooks.add_desc_to_scene = [this](const res::tag& tag) {
		const std::string stem = std::filesystem::path(std::string(tag.relative())).stem().string();
		m_pending_desc_handle = m_services.resources()->require<desc::desc_base>(tag);
		m_pending_desc_tag = tag;
		m_pending_desc_key = m_scene_editor.make_unique_key(stem.empty() ? "Asset" : stem);
		editor_layer->register_implicit("edt/drop_loading", [this] { return poll_pending_desc(); });
	};
	for (auto& kind : register_builtin_asset_kinds(m_context->asset_kinds(), std::move(asset_hooks))) m_registrations.push_back(std::move(kind));
	m_asset_browser_panel->set_on_file_double_clicked([this](const std::filesystem::path& res_rel) {
		m_context->asset_kinds().open(*m_context, res::tag::make(res_rel.generic_string()));
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
	m_game_view_panel->set_registry_getter([this]() -> entt::registry* {
		auto ctx_mgr = m_services.ctx_manager.lock();
		const std::string* active = m_state.active_world_name();
		if (!ctx_mgr || !active) return nullptr;
		auto& level = ctx_mgr->level_set();
		const auto& name = *active;
		return level.has(name) ? &level.get(name).state() : nullptr;
	});
	m_game_view_panel->set_all_registries([this](const game_view_panel::registry_visitor& visit) {
		if (auto ctx_mgr = m_services.ctx_manager.lock()) {
			ctx_mgr->level_set().for_each([&visit](scn::world_context& world) { visit(world.state()); });
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
	add_panel(m_problems_panel, dock_slot::bottom_right, true);
	m_blackboard_panel = std::make_shared<blackboard_panel>();
	m_blackboard_panel->set_manager_getter([this] { return m_services.ctx_manager.lock(); });
	add_panel(m_blackboard_panel, dock_slot::bottom_right, false);
	add_panel(m_asset_browser_panel, dock_slot::bottom_left, true);
	add_panel(m_prefab_panel, dock_slot::floating, false);

	auto& wnd_sys = data.require<wnd::window_system>();
	m_exit_action = [&wnd_sys] {
		if (auto win = wnd_sys.get_active_window())
			win->shutdown();
	};

	m_state.set_save_handlers(
		[this] { return m_level_ctrl.quick_save_level(); },
		[this](const res::tag& target) { return m_level_ctrl.save_level_as(std::string(target.relative())); });

	file_menu_hooks hooks;
	hooks.has_scene = [this] { return m_state.editor_tag.is_valid(); };
	hooks.is_dirty = [this] { return m_state.is_dirty(); };
	hooks.level_path = [this] {
		return m_state.level_tag.is_valid() ? std::string(m_state.level_tag.relative()) : std::string{};
	};
	hooks.new_level = [this] { m_level_ctrl.new_level(); };
	hooks.quick_save = [this] { return m_state.save().saved; };
	hooks.save_level_as = [this](const std::string& rel_path) {
		if (rel_path.find("://") != std::string::npos) {
			egLOG_ERROR("Editor/Save", "Level not saved: '{}' must be a path inside the project, without a protocol", rel_path);
			return false;
		}
		return m_state.save_as(res::tag::make(rel_path)).saved;
	};
	hooks.discard_changes = [this] { m_state.mark_clean(); };
	hooks.load_level = [this](const std::filesystem::path& abs_path) {
		const auto relative = abs_path.lexically_relative(m_res.get_resources_path());
		begin_level_load(res::tag::make(relative.string()));
		editor_layer->register_implicit("edt/load_level", [this] { return poll_level_load(); });
	};
#if SNK_HAS_ASSIMP
	hooks.import_model = [this](const std::filesystem::path& abs_path) { start_import(abs_path); };
#endif
	hooks.exit = m_exit_action;
	hooks.resources_path = m_res.get_resources_path();
	hooks.open_project = start_editor_on;
	// From disk, not through res::: a recursive copy into a folder that no mount covers yet.
	hooks.create_project = [](const std::filesystem::path& folder, const std::string& name) {
		return prj::create_project(*cfg_edt_data_path / "templates" / "projects" / "empty", folder, name);
	};
	// Next to the open project, else where the editor was started.
	hooks.projects_path = data.has_value<prj::project_context>() ? data.require<prj::project_context>().root.parent_path()
		: std::filesystem::current_path();
	hooks.recent_projects = edt::recent_projects_besides(m_res, data.has_value<prj::project_context>()
		? std::optional<std::filesystem::path>{ data.require<prj::project_context>().manifest_file } : std::nullopt);
	m_file_menu = std::make_unique<file_menu>(m_context->dialogs(), m_context->actions(), std::move(hooks));
	for (auto& menu : register_builtin_menus(m_context->actions())) m_registrations.push_back(std::move(menu));
	m_file_menu->register_actions();
	m_file_menu->attach_window_close(wnd_sys);

	game_builder_options build_options;
	if (data.has_value<prj::project_context>()) {
		const auto& project = data.require<prj::project_context>();
		game_project game{ project.root, {} };
		for (const auto& module : project.data.modules) {
			if (module.kind == prj::module_kind::runtime) game.modules.push_back(module.name);
		}
		build_options.project = [game] { return std::optional<game_project>{ game }; };
		build_options.settings = [snk_dir = project.snk_dir()] { return load_build_settings(snk_dir); };
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
	m_game_builder = std::make_unique<game_builder>(m_context->jobs(), m_context->problems(), m_context->dialogs(),
		m_context->actions(), std::move(build_options));

	// A project held back for its engine version: nothing of it is mounted, and no start level is loaded either.
	const bool project_held = data.has_value<prj::held_project>();
	if (project_held) {
		auto& held = data.require<prj::held_project>();
		ask_about_held_project(m_context->dialogs(), held, m_exit_action, [this, manifest = held.manifest_file] {
			const auto outcome = m_context->actions().invoke("file.open_project", boost::json::object{
				{ "path", utf8_of(manifest) }, { "allow_engine_version_mismatch", true }, { "discard_unsaved", true } });
			if (!outcome.ok) {
				m_context->dialogs().message(message_level::error, std::format("The project was not opened: {}.", outcome.error));
			}
		});
	}

	std::string startup_level = cfg_startup_level;
	if (startup_level.empty() && data.has_value<prj::project_context>()) {
		startup_level = data.require<prj::project_context>().data.start_scene;
	}
	if (!project_held && !startup_level.empty()) {
		// On the first frame: the host loads the project's game modules after init (#428), and the level may use their types.
		const res::tag level = startup_level.find("://") != std::string::npos ? res::tag{ startup_level } : res::tag::make(startup_level);
		editor_layer->register_implicit("edt/load_level", begin_then_poll([this, level] { begin_level_load(level); },
			[this] {
				if (poll_level_load()) {
					return true;
				}
				// Begun in a frame, the load ends a frame later, and closing its modal focuses the top panel. The
				// editor starts with no panel focused, as when the load began before the first frame (#437).
				ImGui::SetWindowFocus(nullptr);
				return false;
			}));
	}

	if (cfg_agent_server) {
		agent_server_options options;
		const int port = cfg_agent_server_port;
		options.port = (port > 0 && port <= 65535) ? static_cast<std::uint16_t>(port) : std::uint16_t{ 0 };
		const std::string project_dir = cfg_project_dir;
		options.project_dir = agent_project_dir(project_dir, data.has_value<prj::project_context>()
			? std::optional<std::filesystem::path>{ data.require<prj::project_context>().root } : std::nullopt);
		m_agent_server = std::make_unique<agent_server>(data.require<cmd::command_registry>(), data.require<ds::task_system>(), std::move(options));
	}
}

void edt::editor_system::create_panels(bool capture_log)
{
	m_hierarchy_panel = std::make_shared<scene_hierarchy_panel>();
	m_inspector_panel = std::make_shared<inspector_panel>();
	m_inspector_panel->set_inspector_registry(&m_context->inspectors());
	m_console_panel = std::make_shared<console_panel>();
	m_asset_browser_panel = std::make_shared<asset_browser_panel>();
	m_asset_browser_panel->set_layers(m_res.get_resource_layers());

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
	m_viewport_panel->set_registry_getter([this]() -> entt::registry* {
		auto ctx_mgr = m_services.ctx_manager.lock();
		const std::string* active = m_state.active_world_name();
		if (!ctx_mgr || !active) return nullptr;
		const auto& name = *active;
		auto& level = ctx_mgr->level_set();
		if (!level.has(name)) return nullptr;
		return &level.get(name).state();
	});
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
		ctx_mgr->set_on_game_layer_change([this](scn::context_manager::game_layer_change change) {
			on_game_layer_change(change == scn::context_manager::game_layer_change::before);
		});
	}
}

void edt::editor_system::on_game_layer_change(bool before)
{
	if (before) {
		// Nothing of the editor may refer to the old layer once it goes: the preview set, the loads in flight, the level.
		close_prefab_editor();
		m_pending_desc_handle = {};
		m_level_after_layer_change = res::tag{};
		if (m_loading_level) {
			m_level_after_layer_change = m_state.level_tag;
			m_loading_level = false;
			m_loading_level_handle = {};
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
			if (exported_tag.is_valid() && m_state.editor_tag.is_valid()) {
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

void edt::editor_system::begin_level_load(const res::tag& original_tag)
{
	m_level_ctrl.unwatch_level();

	std::string stem = std::filesystem::path(std::string(original_tag.relative())).stem().string();
	m_state.editor_tag = res::tag{ "memory://editor/" + stem + ".desc" };
	m_state.level_tag = original_tag;

	// Through the resolver: a level in any res:// layer (a lower content root, the engine) is found.
	auto& game_resources = *m_services.resources();
	if (const auto raw = game_resources.fetch_data(original_tag)) {
		raw->wait();
		if (raw->is_ready()) {
			m_services.writable_resources()->store(m_state.editor_tag, raw->data);
		}
	}

	m_loading_level_handle = game_resources.require<scn::level_desc>(m_state.editor_tag);
	m_loading_level = true;
}

bool edt::editor_system::poll_level_load()
{
	if (m_loading_level_handle.has_error()) {
		egLOG_ERROR("Editor/Load", "Failed to load level '{}'", m_state.editor_tag.view());
		m_loading_level = false;
		m_loading_level_handle = {};
		return false;
	}

	if (!m_loading_level_handle.is_ready()) {
		ImGui::OpenPopup("Loading Level##edt_load");
		if (ImGui::BeginPopupModal("Loading Level##edt_load", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
		{
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
				ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				m_loading_level = false;
				m_loading_level_handle = {};
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

	m_level_ctrl.finish_level_load(m_loading_level_handle);
	m_loading_level = false;
	m_loading_level_handle = {};
	return false;
}

bool edt::editor_system::poll_pending_desc()
{
	// Dropped when the game layer changed under the load.
	if (!m_pending_desc_handle.is_valid()) return false;
	if (m_pending_desc_handle.has_error()) {
		egLOG_ERROR("Editor/Drop", "Failed to load desc '{}'", m_pending_desc_tag.view());
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

	m_level_ctrl.add_desc_to_scene(m_pending_desc_handle, m_pending_desc_key);
	m_pending_desc_handle = {};
	return false;
}

// ─── Prefab editor (EPIC-25) ────────────────────────────────────────────────

void edt::editor_system::open_prefab_for_edit(const res::tag& tag)
{
	prefab_document& prefab = m_context->documents().open_prefab(tag);
	if (prefab.is_open()) {
		m_prefab_panel->set_context(&prefab);
		m_prefab_panel->set_open(true);
	}
}

void edt::editor_system::close_prefab_editor()
{
	auto& documents = m_context->documents();
	if (prefab_document* prefab = documents.preview()) {
		documents.close(prefab->id());
	}
	m_prefab_panel->set_context(nullptr);
	m_prefab_panel->set_open(false);
}
