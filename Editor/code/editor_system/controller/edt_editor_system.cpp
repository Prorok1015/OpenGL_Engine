#include "edt_editor_system.h"
#include <snk/editor/edt_editor_context.h>
#include <snk/common/engine_assert.h>
#include "edt_init_helpers.h"
#include "edt_selection_wiring.h"
#include "edt_prefab_editor_context.h"

#include "edt_editor_layer.h"
#include "edt_scene_hierarchy_panel.h"
#include "edt_inspector_panel.h"
#include "edt_override_edit.h"
#include "edt_viewport_panel.h"
#include "edt_game_view_panel.h"
#include "edt_console_panel.h"
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
#include "edt_main_menu.h"
#include <snk/application/app_deterministic_time.h>
#include <snk/command/cmd_registry.h>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
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
#include <snk/config/cfg_api.h>
#include <imgui.h>
#include <entt/entt.hpp>
#include <fstream>

namespace {
	// Level opened at startup, e.g. --set editor/startup_level levels/smoke_level.desc (automation, headless runs).
	CFG_VAR_DEF_STRING(cfg_startup_level, "editor/startup_level", "");
	// Opens the Game view at startup (automation); otherwise it is toggled from the View menu.
	CFG_VAR_DEF_BOOL(cfg_show_game_view, "editor/show_game_view", false);
}

edt::editor_system::editor_system(desc::desc_system& desc_system_, res::resource_system& res_sys, rnd::render_system& rnd_sys, gui::gui_system& gui_sys)
	: m_scene_editor(m_state)
	, m_world_ctrl(m_state, res_sys, desc_system_)
	, m_level_ctrl(m_state, res_sys, desc_system_, m_scene_editor, m_world_ctrl)
	, desc_system(desc_system_)
	, m_res(res_sys)
	, m_rnd(rnd_sys)
	, m_gui(gui_sys)
	, m_model_importer(std::make_unique<edt::model_importer>(desc_system_, res_sys, rnd_sys))
	, m_asset_exporter(std::make_unique<edt::asset_exporter>(res_sys))
	, m_export_dialog(std::make_unique<edt::asset_export_dialog>(*m_asset_exporter))
{
}

edt::editor_system::~editor_system() = default;

void edt::editor_system::init(ds::app_data_storage& data)
{
	m_rnd.get_texture_manager().generate_texture(res::tag(res::tag::memory, "__black"), {1,1}, rnd::driver::texture_header::TYPE::RGB8, {0, 0, 0});
	m_rnd.get_texture_manager().generate_texture(res::tag(res::tag::memory, "__red"),   {1,1}, rnd::driver::texture_header::TYPE::RGB8, {255, 0, 0});

	m_res.warmup<res::text_resource>(res::tag::make("templates/level.desc"));
	m_res.warmup<res::text_resource>(res::tag::make("templates/world.desc"));

	editor_layer = std::make_shared<edt::editor_layer>();
	m_gui.push_layer(editor_layer);

	// Resolve services from data storage → shared state
	m_state.ctx_manager = data.require_shared<scn::context_manager>();
	// The editor scene is authored, not played: only run_in_edit systems tick (transforms, camera, animation preview).
	m_state.ctx_manager.lock()->level_set().set_mode(ecs::run_mode::edit);
	m_state.sfactory  = &data.require<ecs::system_factory>();
	m_state.assembler = &data.require<scn::ecs_assembler>();
	ASSERT_MSG(!data.has_value<edt::editor_context>(), "editor_system::init called twice: editor_context is already registered");
	m_context = &data.construct<edt::editor_context>(desc_system, m_res, m_rnd, m_gui, *m_state.assembler, *m_state.sfactory,
		data.require<scn::context_manager>(), data.require<ds::task_system>(), data.require<cmd::command_registry>());
	editor_layer->set_context(*m_context);
	m_export_dialog->set_jobs(m_context->jobs());
	m_context->dialogs().set_registrar([this](std::string_view id, std::function<bool()> draw) {
		editor_layer->register_implicit(id, std::move(draw));
	});
	// The panels own input routing: whichever scene image is hovered feeds its context, nothing else does.
	m_input_router = &data.require<inp::input_router>();
	m_input_router->set_default_target(std::nullopt);

	// Create panels
	init_helpers::setup_panels(
		m_hierarchy_panel, m_inspector_panel, m_viewport_panel,
		m_console_panel, m_asset_browser_panel,
		*this, m_log_sink,
		// Load order, absolute paths and timings in the log text differ between runs and would show in screenshots.
		!(data.has_value<app::deterministic_time>() && data.require<app::deterministic_time>().enabled()));
	for (auto& renderer : register_desc_component_renderers(m_context->inspectors())) m_registrations.push_back(std::move(renderer));
	m_inspector_panel->set_desc_system(&desc_system);
	{
		// One composed view of the instances for the inspector and the hierarchy; edits and pushes invalidate it.
		auto view_cache = std::make_shared<override_edit::view_cache>(*m_state.assembler, m_state);
		m_inspector_panel->set_view_cache(view_cache);
		m_hierarchy_panel->set_view_cache(std::move(view_cache));
	}
	init_helpers::setup_viewport_panel(
		m_viewport_panel, *m_input_router, *this, m_state, m_rnd, m_gui, m_res);

	// Wire all controller + panel callbacks
	init_helpers::wire_controller_callbacks(
		*this, m_scene_editor, m_world_ctrl, m_level_ctrl, *editor_layer, m_state);
	init_helpers::wire_hierarchy_callbacks(
		*m_hierarchy_panel, *m_inspector_panel, *m_viewport_panel,
		m_scene_editor, m_world_ctrl, m_state);
	m_context->selection().set_registry_provider([this](const std::string& world) -> entt::registry* {
		auto ctx_mgr = m_state.ctx_manager.lock();
		scn::world_context* found = ctx_mgr ? ctx_mgr->level_set().find(world) : nullptr;
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
		m_pending_desc_handle = m_res.require<desc::desc_base>(tag);
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
		auto ctx_mgr = m_state.ctx_manager.lock();
		const std::string* active = m_state.active_world_name();
		if (!ctx_mgr || !active) return nullptr;
		auto& level = ctx_mgr->level_set();
		const auto& name = *active;
		return level.has(name) ? &level.get(name).state() : nullptr;
	});
	m_game_view_panel->set_all_registries([this](const game_view_panel::registry_visitor& visit) {
		if (auto ctx_mgr = m_state.ctx_manager.lock()) {
			ctx_mgr->level_set().for_each([&visit](scn::world_context& world) { visit(world.state()); });
		}
	});
	m_game_view_panel->set_input_router(m_input_router, [this] { return active_play_input_target(m_state); });
	add_panel(m_game_view_panel, dock_slot::center, cfg_show_game_view);
	// viewport.capture names the two views by what the panels show; the engine knows only these ids.
	if (data.has_value<rnd::capture_service>()) {
		auto& capture = data.require<rnd::capture_service>();
		m_registrations.push_back(capture.add_target("scene", [this] {
			return m_rnd.get_texture_manager().find(viewport_panel::render_target_tag());
		}));
		m_registrations.push_back(capture.add_target("game", [this]() -> rnd::driver::texture_interface* {
			const auto& target = m_game_view_panel->render_target();
			return target ? m_rnd.get_texture_manager().find(*target) : nullptr;
		}));
	}
	add_panel(m_console_panel, dock_slot::bottom_right, true);
	add_panel(m_asset_browser_panel, dock_slot::bottom_left, true);
	add_panel(m_prefab_panel, dock_slot::floating, false);

	auto& wnd_sys = data.require<wnd::window_system>();
	m_exit_action = [&wnd_sys] {
		if (auto win = wnd_sys.get_active_window())
			win->shutdown();
	};

	file_menu_hooks hooks;
	hooks.has_scene = [this] { return m_state.editor_tag.is_valid(); };
	hooks.is_dirty = [this] { return m_state.is_dirty; };
	hooks.level_path = [this] {
		return m_state.level_tag.is_valid() ? std::string(m_state.level_tag.relative()) : std::string{};
	};
	hooks.new_level = [this] { m_level_ctrl.new_level(); };
	hooks.quick_save = [this] { return m_level_ctrl.quick_save_level(); };
	hooks.save_level_as = [this](const std::string& rel_path) { return m_level_ctrl.save_level_as(rel_path); };
	hooks.discard_changes = [this] { m_state.is_dirty = false; };
	hooks.load_level = [this](const std::filesystem::path& abs_path) {
		const auto relative = abs_path.lexically_relative(res::resource_system::get_resources_path());
		begin_level_load(res::tag::make(relative.string()));
		editor_layer->register_implicit("edt/load_level", [this] { return poll_level_load(); });
	};
	hooks.import_model = [this](const std::filesystem::path& abs_path) { start_import(abs_path); };
	hooks.exit = m_exit_action;
	hooks.resources_path = res::resource_system::get_resources_path();
	m_file_menu = std::make_unique<file_menu>(m_context->dialogs(), m_context->actions(), std::move(hooks));
	for (auto& menu : register_builtin_menus(m_context->actions())) m_registrations.push_back(std::move(menu));
	m_file_menu->register_actions();
	m_file_menu->attach_window_close(wnd_sys);

	if (const std::string startup_level = cfg_startup_level; !startup_level.empty()) {
		begin_level_load(startup_level.find("://") != std::string::npos ? res::tag{ startup_level } : res::tag::make(startup_level));
		editor_layer->register_implicit("edt/load_level", [this] { return poll_level_load(); });
	}
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
				m_pending_desc_handle = m_res.require<desc::desc_base>(exported_tag);
				m_pending_desc_tag = exported_tag;
				m_pending_desc_key = m_scene_editor.make_unique_key(name_str.empty() ? "ImportedModel" : name_str);
				editor_layer->register_implicit("edt/export_add", [this] { return poll_pending_desc(); });
			}
			return false;
		}
		return true;
	}

	if (m_import_token) {
		return poll_import_progress();
	}
	return false;
}

void edt::editor_system::start_import(const std::filesystem::path& abs_path)
{
	if (m_import_token) return;

	m_import_token = std::make_unique<edt::async_import_token>();
	m_import_token->display_name = abs_path.filename().string();
	// compute lane: assimp parsing and mesh building are CPU-bound; the io lane has two threads for blocking files.
	m_import_token->job = m_context->jobs().start(ds::lane::compute,
		{ "Import " + m_import_token->display_name, "import" }, m_lifetime,
		[importer = m_model_importer.get(), abs_path](ds::task_context& ctx) -> edt::import_intermediate {
			ctx.throw_if_stopped();
			return importer->import_background(abs_path);
		},
		[this](ds::task_result<edt::import_intermediate>& result) {
			if (!m_import_token) return;
			m_import_token->finished = true;
			if (result.ok()) {
				m_import_token->intermediate = std::move(*result.value);
			} else {
				m_import_token->failure = result.error;
			}
		});
	editor_layer->register_implicit("edt/import_model", [this] { return poll_import_pipeline(); });
}

bool edt::editor_system::poll_import_progress()
{
	auto& token = *m_import_token;
	// Not running and not finished: the job was cancelled and its result discarded.
	const bool waiting = !token.finished && m_context->jobs().is_running(token.job);

	if (waiting) {
		ImGui::OpenPopup("Importing##edt_import");
		if (ImGui::BeginPopupModal("Importing##edt_import", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
		{
			token.spinner_t += ImGui::GetIO().DeltaTime;
			const char* spinners[] = { "|", "/", "-", "\\" };
			int idx = static_cast<int>(token.spinner_t * 8.f) % 4;
			ImGui::Text("%s  Importing '%s'...", spinners[idx], token.display_name.c_str());
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

	if (!token.intermediate) {
		if (!token.failure.empty()) {
			egLOG("Editor/Import", "Async import failed: {}", token.failure);
		}
		m_import_token.reset();
		return false;
	}

	auto intermediate = std::move(*token.intermediate);
	m_import_token.reset();

	if (!intermediate.error.empty()) {
		egLOG("Editor/Import", "Async import failed: {}", intermediate.error);
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

// ─── Level load ─────────────────────────────────────────────────────────────

void edt::editor_system::begin_level_load(const res::tag& original_tag)
{
	if (m_state.editor_tag.is_valid()) {
		m_res.unwatch(m_state.editor_tag, &m_level_ctrl);
	}

	std::string stem = std::filesystem::path(std::string(original_tag.relative())).stem().string();
	m_state.editor_tag = res::tag{ "memory://editor/" + stem + ".desc" };
	m_state.level_tag = original_tag;

	auto abs_path = res::resource_system::get_resources_path() / std::string(original_tag.relative());
	std::ifstream in(abs_path, std::ios::binary | std::ios::ate);
	if (in) {
		auto size = (std::streamsize)in.tellg();
		in.seekg(0);
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		in.read(reinterpret_cast<char*>(bytes.data()), size);
		m_res.store(m_state.editor_tag, bytes);
	}

	m_loading_level_handle = m_res.require<scn::level_desc>(m_state.editor_tag);
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

// ─── Editor camera ──────────────────────────────────────────────────────────

void edt::editor_system::save_editor_camera_state(entt::registry& reg)
{
	init_helpers::save_camera_state(reg, m_camera_state);
}

void edt::editor_system::inject_editor_camera(entt::registry& reg)
{
	init_helpers::inject_camera(reg, m_camera_state);
}

// ─── Prefab editor (EPIC-25) ────────────────────────────────────────────────

void edt::editor_system::open_prefab_for_edit(const res::tag& tag)
{
	if (!m_prefab_ctx) {
		m_prefab_ctx = std::make_unique<prefab_editor_context>(m_state, m_res, desc_system);
	}

	if (m_prefab_ctx->is_open()) {
		m_prefab_ctx->close();
	}

	m_prefab_ctx->open(tag);

	if (m_prefab_ctx->is_open()) {
		m_prefab_panel->set_context(m_prefab_ctx.get());
		m_prefab_panel->set_open(true);
	}
}

void edt::editor_system::close_prefab_editor()
{
	if (m_prefab_ctx) {
		m_prefab_ctx->close();
	}
	m_prefab_panel->set_context(nullptr);
	m_prefab_panel->set_open(false);
}

