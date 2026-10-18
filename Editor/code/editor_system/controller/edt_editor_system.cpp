#include "edt_editor_system.h"
#include "edt_init_helpers.h"
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
#include "edt_component_ui_registry.h"
#include "edt_model_importer.h"
#include "edt_asset_exporter.h"
#include "edt_asset_export_dialog.h"
#include "edt_async_import_task.h"
#include "edt_input_manager.h"
#include "edt_file_dialog.h"
#include "desc_base.hpp"
#include "desc_system.h"
#include "res_system.h"
#include "rnd_render_system.h"
#include "gui_system.h"
#include "inp_input_system.h"
#include "inp_ecs_input_manager.h"
#include "level/scn_level_desc.h"
#include "level/scn_level_manager.h"
#include "level/scn_level.h"
#include "level/scn_world.h"
#include "level/scn_prefab_desc.h"
#include <snk/window/wnd_window_system.h>
#include "resources/res_resource_text.h"
#include <cfg_api.h>
#include <imgui.h>
#include <entt/entt.hpp>
#include <future>
#include <fstream>

namespace {
	// Level opened at startup, e.g. --set editor/startup_level levels/smoke_level.desc (automation, headless runs).
	CFG_VAR_DEF_STRING(cfg_startup_level, "editor/startup_level", "");
	// Opens the Game view at startup (automation); otherwise it is toggled from the View menu.
	CFG_VAR_DEF_BOOL(cfg_show_game_view, "editor/show_game_view", false);
}

edt::editor_system::editor_system(desc::desc_system& desc_system_, res::resource_system& res_sys, rnd::render_system& rnd_sys, gui::gui_system& gui_sys)
	: desc_system(desc_system_)
	, m_res(res_sys)
	, m_rnd(rnd_sys)
	, m_gui(gui_sys)
	, m_file_dialog(std::make_unique<edt::file_dialog>())
	, m_model_importer(std::make_unique<edt::model_importer>(desc_system_, res_sys, rnd_sys))
	, m_asset_exporter(std::make_unique<edt::asset_exporter>(res_sys))
	, m_export_dialog(std::make_unique<edt::asset_export_dialog>(*m_asset_exporter))
	, m_component_ui_registry(std::make_unique<edt::component_ui_registry>())
	, m_scene_editor(m_state)
	, m_world_ctrl(m_state, res_sys, desc_system_)
	, m_level_ctrl(m_state, res_sys, desc_system_, m_scene_editor, m_world_ctrl)
{
	input = std::make_shared<edt::input_manager>();
}

edt::editor_system::~editor_system() = default;

void edt::editor_system::init(ds::app_data_storage& data)
{
	m_file_dialog->set_current_path(res::resource_system::get_resources_path());
	m_rnd.get_texture_manager().generate_texture(res::tag(res::tag::memory, "__black"), {1,1}, rnd::driver::texture_header::TYPE::RGB8, {0, 0, 0});
	m_rnd.get_texture_manager().generate_texture(res::tag(res::tag::memory, "__red"),   {1,1}, rnd::driver::texture_header::TYPE::RGB8, {255, 0, 0});

	m_res.warmup<res::text_resource>(res::tag::make("templates/level.desc"));
	m_res.warmup<res::text_resource>(res::tag::make("templates/world.desc"));

	editor_layer = std::make_shared<edt::editor_layer>();
	m_gui.push_layer(editor_layer);

	// Resolve services from data storage → shared state
	m_state.lvl_manager = data.require_shared<scn::level_manager>();
	// The editor scene is authored, not played: only run_in_edit systems tick (transforms, camera, animation preview).
	m_state.lvl_manager.lock()->get_level().set_mode(ecs::run_mode::edit);
	m_state.sfactory  = &data.require<ecs::system_factory>();
	m_state.assembler = &data.require<scn::ecs_assembler>();
	auto& inp_sys = data.require<inp::input_system>();
	inp_sys.push_input_layer(inp_sys.get_focused_window(), inp::input_layer{ input, true });
	ecs_input = data.require_shared<inp::ecs_input_manager>();

	// Create panels
	init_helpers::setup_panels(
		m_hierarchy_panel, m_inspector_panel, m_viewport_panel,
		m_console_panel, m_asset_browser_panel,
		*m_component_ui_registry, *this);
	m_inspector_panel->set_desc_system(&desc_system);
	{
		// One composed view of the instances for the inspector and the hierarchy; edits and pushes invalidate it.
		auto view_cache = std::make_shared<override_edit::view_cache>(*m_state.assembler, m_state);
		m_inspector_panel->set_view_cache(view_cache);
		m_hierarchy_panel->set_view_cache(std::move(view_cache));
	}
	init_helpers::setup_viewport_panel(
		m_viewport_panel, input, ecs_input, *this, m_state, m_rnd, m_gui, m_res);

	// Wire all controller + panel callbacks
	init_helpers::wire_controller_callbacks(
		*this, m_scene_editor, m_world_ctrl, m_level_ctrl, *editor_layer,
		*m_hierarchy_panel, *m_inspector_panel, *m_viewport_panel, m_state);
	init_helpers::wire_hierarchy_callbacks(
		*m_hierarchy_panel, *m_inspector_panel, *m_viewport_panel,
		m_scene_editor, m_world_ctrl, m_state);

	// Create prefab editor panel (EPIC-25)
	m_prefab_panel = std::make_shared<prefab_editor_panel>(m_rnd, m_gui);
	m_prefab_panel->set_on_close([this] { close_prefab_editor(); });
	m_prefab_panel->set_ecs_input_manager(ecs_input);
	m_prefab_panel->set_editor_input(input);

	// Wire asset browser double-click for prefab editing
	m_asset_browser_panel->set_on_file_double_clicked([this](const std::filesystem::path& res_rel) {
		std::string filename = res_rel.filename().string();
		if (filename.ends_with(".prefab.desc")) {
			auto tag = res::tag::make(res_rel.generic_string());
			open_prefab_for_edit(tag);
		}
	});

	// Register panels with manager
	auto& pm = editor_layer->get_panel_manager();
	pm.add_panel(m_hierarchy_panel);
	pm.add_panel(m_inspector_panel);
	pm.add_panel(m_viewport_panel);
	m_game_view_panel = std::make_shared<game_view_panel>(m_rnd, m_gui);
	m_game_view_panel->set_registry_getter([this]() -> entt::registry* {
		auto lvl_mgr = m_state.lvl_manager.lock();
		const std::string* active = m_state.active_world_name();
		if (!lvl_mgr || !active) return nullptr;
		auto& level = lvl_mgr->get_level();
		const auto& name = *active;
		return level.has_world(name) ? &level.get_world(name).state() : nullptr;
	});
	m_game_view_panel->set_all_registries([this](const game_view_panel::registry_visitor& visit) {
		if (auto lvl_mgr = m_state.lvl_manager.lock()) {
			lvl_mgr->get_level().for_each_world([&visit](scn::world& world) { visit(world.state()); });
		}
	});
	m_game_view_panel->set_open(cfg_show_game_view);
	pm.add_panel(m_game_view_panel);
	pm.add_panel(m_console_panel);
	pm.add_panel(m_asset_browser_panel);
	pm.add_panel(m_prefab_panel);

	auto& wnd_sys = data.require<wnd::window_system>();
	m_exit_action = [&wnd_sys] {
		if (auto win = wnd_sys.get_active_window())
			win->shutdown();
	};

	// Wire dockspace menu actions
	init_helpers::wire_dockspace_menus(*editor_layer, m_level_ctrl, *this, m_state, *m_file_dialog, m_exit_action);

	if (const std::string startup_level = cfg_startup_level; !startup_level.empty()) {
		begin_level_load(startup_level.find("://") != std::string::npos ? res::tag{ startup_level } : res::tag::make(startup_level));
		// load_level() polls the pending load (and skips the file dialog) while m_loading_level is set.
		editor_layer->register_implicit("edt/load_level", [this] { return load_level(); });
	}
}

// ─── Import pipeline ────────────────────────────────────────────────────────

bool edt::editor_system::show_file_dialog()
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

	bool is_open = true;
	if (m_file_dialog->show("Import Model", &is_open)) {
		auto abs_path = m_file_dialog->get_selected_path();
		std::string display = abs_path.filename().string();

		m_import_token = std::make_unique<edt::async_import_token>();
		m_import_token->display_name = display;
		m_import_token->future = std::async(std::launch::async,
			[this, abs_path]() -> edt::import_intermediate {
				return m_model_importer->import_background(abs_path);
			});
		return true;
	}
	return is_open;
}

bool edt::editor_system::poll_import_progress()
{
	auto& token = *m_import_token;
	bool finished = token.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;

	if (!finished) {
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

	auto intermediate = token.future.get();
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

bool edt::editor_system::load_level()
{
	if (m_loading_level) {
		return poll_level_load();
	}

	bool is_open = false;
	if (auto level_manager = m_state.lvl_manager.lock()) {
		is_open = true;
		if (m_file_dialog->show("Load Level", &is_open)) {
			auto relateve = m_file_dialog->get_selected_path().lexically_relative(m_file_dialog->get_base_path());
			begin_level_load(res::tag::make(relateve.string()));
			return true;
		}
	}
	return is_open;
}

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

