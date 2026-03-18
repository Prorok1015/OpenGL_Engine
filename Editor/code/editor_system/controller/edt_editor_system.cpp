#include "edt_editor_system.h"
#include "res_system.h"
#include "level/scn_level_desc.h"
#include "rnd_render_system.h"
#include "scn_camera_component.hpp"
#include "scn_camera_controller_component.hpp"
#include "ecs_event.hpp"
#include "inp_input_system.h"
#include "edt_input_manager.h"
#include "gui_system.h"
#include <imgui.h>
#include "level/scn_prefab_desc.h"

#include "level/scn_world.h"
#include "level/scn_level.h"
#include "level/scn_level_manager.h"
#include "wnd_window_system.h"

#include "edt_component_renderers.h"
#include <entt/entt.hpp>
#include <future>
#include "eng_transform_3d.hpp"
#include "resources/res_resource_text.h"
#include <fstream>
#include <algorithm>

edt::editor_system::editor_system(desc::desc_system& desc_system_, res::resource_system& res_sys, rnd::render_system& rnd_sys, gui::gui_system& gui_sys)
	: desc_system(desc_system_)
	, m_res(res_sys)
	, m_rnd(rnd_sys)
	, m_gui(gui_sys)
	, m_model_importer(desc_system_, res_sys, rnd_sys)
	, m_asset_exporter(res_sys)
	, m_export_dialog(m_asset_exporter)
	, m_scene_editor(m_state)
	, m_world_ctrl(m_state, res_sys, desc_system_)
	, m_level_ctrl(m_state, res_sys, desc_system_, m_scene_editor, m_world_ctrl)
{
	input = std::make_shared<edt::input_manager>();
}

edt::editor_system::~editor_system() = default;

void edt::editor_system::init(ds::app_data_storage& data)
{
	file_dialog.set_current_path(res::resource_system::get_resources_path());
	m_rnd.get_texture_manager().generate_texture(res::tag(res::tag::memory, "__black"), {1,1}, rnd::driver::texture_header::TYPE::RGB8, {0, 0, 0});
	m_rnd.get_texture_manager().generate_texture(res::tag(res::tag::memory, "__red"),   {1,1}, rnd::driver::texture_header::TYPE::RGB8, {255, 0, 0});

	m_res.warmup<res::text_resource>(res::tag::make("templates/level.desc"));
	m_res.warmup<res::text_resource>(res::tag::make("templates/world.desc"));

	editor_layer = std::make_shared<edt::editor_layer>();
	m_gui.push_layer(editor_layer);

	editor_layer->register_tool("File/Import...", [this] { return show_file_dialog(); });

	// Resolve services from data storage → shared state
	m_state.lvl_manager = data.require_shared<scn::level_manager>();
	m_state.sfactory  = &data.require<ecs::system_factory>();
	m_state.assembler = &data.require<scn::ecs_assembler>();
	auto& inp_sys = data.require<inp::input_system>();
	inp_sys.push_input_layer(inp_sys.get_focused_window(), inp::input_layer{ input, true });
	ecs_input = data.require_shared<inp::ecs_input_manager>();

	// Panels
	m_hierarchy_panel = std::make_shared<scene_hierarchy_panel>();
	m_inspector_panel = std::make_shared<inspector_panel>();
	register_desc_component_renderers(m_component_ui_registry);
	m_inspector_panel->set_component_ui_registry(&m_component_ui_registry);
	m_inspector_panel->set_desc_system(&desc_system);
	m_viewport_panel = std::make_shared<viewport_panel>(m_rnd, m_gui);
	m_viewport_panel->set_registry_getter([this]() -> entt::registry* {
		auto lvl_mgr = m_state.lvl_manager.lock();
		if (!lvl_mgr || m_state.active_world_idx >= (int)m_state.world_names.size()) return nullptr;
		const auto& name = m_state.world_names[m_state.active_world_idx];
		auto& level = lvl_mgr->get_level();
		if (!level.has_world(name)) return nullptr;
		return &level.get_world(name).state();
	});
	m_console_panel = std::make_shared<console_panel>();

	auto console_weak = std::weak_ptr<console_panel>(m_console_panel);
	engine::set_log_callback([console_weak](std::string_view category, engine::log_level level, std::string_view message) {
		auto panel = console_weak.lock();
		if (!panel) return;
		edt::log_level edt_level = edt::log_level::info;
		if (level == engine::log_level::warn)  edt_level = edt::log_level::warning;
		if (level == engine::log_level::error) edt_level = edt::log_level::error;
		panel->add_log(edt_level, std::format("[{}] {}", category, message));
	});

	m_asset_browser_panel = std::make_shared<asset_browser_panel>();
	m_asset_browser_panel->set_base_path(res::resource_system::get_resources_path());

	m_viewport_panel->set_input_manager(input);
	m_viewport_panel->set_ecs_input_manager(ecs_input);
	m_viewport_panel->set_on_asset_dropped([this](const std::string& abs_path) {
		if (!m_state.editor_tag.is_valid()) return;

		auto base = res::resource_system::get_resources_path();
		auto rel  = std::filesystem::path(abs_path).lexically_relative(base);
		res::tag tag = res::tag::make(rel.string());

		const std::string stem = rel.stem().string();

		m_pending_desc_handle = m_res.require<desc::desc_base>(tag);
		m_pending_desc_tag = tag;
		m_pending_desc_key = m_scene_editor.make_unique_key(stem.empty() ? "Asset" : stem);
		editor_layer->register_implicit("edt/drop_loading", [this] { return poll_pending_desc(); });
	});

	// ─── Wire scene_editor callbacks ──────────────────────────────────────
	m_scene_editor.on_before_modify = [this] { clear_selection(); };
	m_scene_editor.on_after_push = [this] { sync_viewport_selection(); };
	m_scene_editor.on_save_camera = [this](entt::registry& reg) { save_editor_camera_state(reg); };
	m_scene_editor.on_inject_camera = [this](entt::registry& reg) { inject_editor_camera(reg); };

	// ─── Wire world_controller callbacks ──────────────────────────────────
	m_world_ctrl.on_save_camera = [this](entt::registry& reg) { save_editor_camera_state(reg); };
	m_world_ctrl.on_inject_camera = [this](entt::registry& reg) { inject_editor_camera(reg); };
	m_world_ctrl.on_world_switched = [this](int idx) {
		m_hierarchy_panel->set_world_desc(&m_state.active_world_desc());
		m_hierarchy_panel->set_world_names(m_state.world_names, idx);
		m_inspector_panel->set_selected_node(nullptr);
		m_hierarchy_panel->set_selected_node(nullptr);
		m_viewport_panel->set_selected_entity(entt::null);
	};

	// ─── Wire level_controller callbacks ──────────────────────────────────
	m_level_ctrl.on_editor_world_reloaded = [] {
		egLOG("Editor", "Editor world reloaded");
	};
	m_level_ctrl.on_need_save_dialog = [this] {
		editor_layer->register_implicit("edt/save_level", [this] { return m_level_ctrl.save_level(); });
	};

	// ─── Wire hierarchy panel callbacks ───────────────────────────────────
	m_hierarchy_panel->set_on_node_selected([this](scn::prefab_desc::prefab_node* node) {
		m_inspector_panel->set_selected_node(node);
		auto lvl_mgr = m_state.lvl_manager.lock();
		if (lvl_mgr && node && m_state.active_world_idx < (int)m_state.world_names.size()) {
			auto& reg = lvl_mgr->get_level().get_world(m_state.world_names[m_state.active_world_idx]).state();
			ecs::entity found = entt::null;
			reg.view<scn::name_component>().each([&](auto ent, const scn::name_component& nc) {
				if (nc.name == node->name) found = ent;
			});
			m_viewport_panel->set_selected_entity(found);
		} else {
			m_viewport_panel->set_selected_entity(entt::null);
		}
	});

	m_hierarchy_panel->set_on_world_changed([this](int idx) {
		m_world_ctrl.switch_to_world(idx);
	});
	m_hierarchy_panel->set_on_create_world([this](const std::string& name) {
		m_world_ctrl.create_world(name);
	});
	m_hierarchy_panel->set_on_create_node([this](const std::string& type_name) {
		m_scene_editor.create_entity(type_name);
	});
	m_hierarchy_panel->set_on_delete_node([this](const std::string& name) {
		m_scene_editor.delete_entity(name);
	});
	m_hierarchy_panel->set_on_rename_node([this](const std::string& /*old_name*/, const std::string& /*new_name*/) {
		m_state.is_dirty = true;
		m_scene_editor.serialize_and_push();
	});
	m_hierarchy_panel->set_on_duplicate_node([this](const std::string& name) {
		m_scene_editor.duplicate_entity(name);
	});
	m_hierarchy_panel->set_on_reparent_node([this](const std::string& node_name, const std::string& new_parent_name, int insert_index) {
		m_scene_editor.reparent_entity(node_name, new_parent_name, insert_index);
	});

	m_inspector_panel->set_on_node_changed([this]() {
		m_state.is_dirty = true;
		m_scene_editor.serialize_and_push();
	});

	m_viewport_panel->set_on_transform_committed(
		[this](const std::string& name, glm::vec3 pos, glm::vec3 rot, glm::vec3 scale) {
			m_scene_editor.on_transform_committed(name, pos, rot, scale);
		});

	auto& pm = editor_layer->get_panel_manager();
	pm.add_panel(m_hierarchy_panel);
	pm.add_panel(m_inspector_panel);
	pm.add_panel(m_viewport_panel);
	pm.add_panel(m_console_panel);
	pm.add_panel(m_asset_browser_panel);

	auto& wnd_sys = data.require<wnd::window_system>();
	m_exit_action = [&wnd_sys] {
		if (auto win = wnd_sys.get_active_window())
			win->shutdown();
	};

	// ─── Wire dockspace menu actions ──────────────────────────────────────
	auto& ds = editor_layer->get_dockspace();
	ds.set_on_new_level([this] {
		if (m_state.is_dirty) {
			m_level_ctrl.m_confirm_action = [this] { m_level_ctrl.new_level(); };
			editor_layer->register_implicit("edt/new_confirm", [this] {
				return m_level_ctrl.show_exit_confirm();
			});
		} else {
			m_level_ctrl.new_level();
		}
	});
	ds.set_on_open_level([this] {
		file_dialog.set_current_path(res::resource_system::get_resources_path());
		file_dialog.set_select_mode(edt::file_dialog::SELECT_MODE::FILES_ONLY);
		file_dialog.clear_extension_filters();
		file_dialog.add_extension_filter(".desc");
		editor_layer->register_implicit("edt/load_level", [this] { return load_level(); });
	});
	ds.set_on_save_level([this] {
		editor_layer->register_implicit("edt/save_level", [this] { return m_level_ctrl.save_level(); });
	});
	ds.set_on_quick_save([this] {
		if (m_state.level_tag.is_valid() && m_state.editor_tag.is_valid()) {
			m_level_ctrl.quick_save_level();
		} else if (m_state.editor_tag.is_valid()) {
			editor_layer->register_implicit("edt/save_level", [this] { return m_level_ctrl.save_level(); });
		}
	});
	ds.set_on_exit([this] {
		if (m_state.is_dirty) {
			m_level_ctrl.m_confirm_action = m_exit_action;
			editor_layer->register_implicit("edt/exit_confirm", [this] {
				return m_level_ctrl.show_exit_confirm();
			});
		} else {
			m_exit_action();
		}
	});
	ds.set_on_import_model([this] {
		file_dialog.set_current_path(std::filesystem::current_path());
		file_dialog.set_select_mode(edt::file_dialog::SELECT_MODE::FILES_ONLY);
		file_dialog.clear_extension_filters();
		file_dialog.add_extension_filter(".glb");
		file_dialog.add_extension_filter(".obj");
		file_dialog.add_extension_filter(".fbx");
		file_dialog.add_extension_filter(".gltf");
		editor_layer->register_implicit("edt/import_model", [this] { return show_file_dialog(); });
	});
}

// ─── Import pipeline (stays here — orchestration with 3 phases) ─────────────

bool edt::editor_system::show_file_dialog()
{
	if (m_export_dialog_active) {
		bool still_open = m_export_dialog.render();
		if (!still_open) {
			m_export_dialog_active = false;
			auto exported_tag = m_export_dialog.get_exported_tag();
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

	if (m_import_token.has_value()) {
		return poll_import_progress();
	}

	bool is_open = true;
	if (file_dialog.show("Import Model", &is_open)) {
		auto abs_path = file_dialog.get_selected_path();
		std::string display = abs_path.filename().string();

		edt::async_import_token token;
		token.display_name = display;
		token.future = std::async(std::launch::async,
			[this, abs_path]() -> edt::import_intermediate {
				return m_model_importer.import_background(abs_path);
			});
		m_import_token = std::move(token);
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

	auto result = m_model_importer.finalize_on_main(std::move(intermediate));
	if (result.prefab) {
		imported_models_list.push_back(result.root_tag);
		egLOG("Editor/Import", "Model imported to memory: {}", result.root_tag.view());
		m_export_dialog.open(std::move(result));
		m_export_dialog_active = true;
		return true;
	}

	egLOG("Editor/Import", "Failed to finalize import");
	return false;
}

// ─── Level load (file dialog + async polling stays in coordinator) ──────────

bool edt::editor_system::load_level()
{
	if (m_loading_level) {
		return poll_level_load();
	}

	bool is_open = false;
	if (auto level_manager = m_state.lvl_manager.lock()) {
		is_open = true;
		if (file_dialog.show("Load Level", &is_open)) {
			auto relateve = file_dialog.get_selected_path().lexically_relative(file_dialog.get_base_path());
			res::tag original_tag = res::tag::make(relateve.string());

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
			return true;
		}
	}
	return is_open;
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

// ─── Editor camera (EPIC-11) ────────────────────────────────────────────────

void edt::editor_system::save_editor_camera_state(entt::registry& reg)
{
	auto view = reg.view<edt::editor_camera_tag, scn::mouse_controller_component>();
	for (auto ent : view) {
		auto& ctrl = reg.get<scn::mouse_controller_component>(ent);
		m_camera_state.position = ctrl.position;
		m_camera_state.rotation = ctrl.rotation;
		m_camera_state.distance = ctrl.distance;
		break;
	}
}

void edt::editor_system::inject_editor_camera(entt::registry& reg)
{
	for (auto ent : reg.view<edt::editor_camera_tag>())
		reg.destroy(ent);

	entt::entity cam = reg.create();
	reg.emplace<edt::editor_camera_tag>(cam);

	scn::camera_component cam_comp;
	cam_comp.fov           = m_camera_state.fov;
	cam_comp.near_distance = m_camera_state.near_distance;
	cam_comp.far_distance  = m_camera_state.far_distance;
	cam_comp.texture       = res::tag{ edt::editor_camera_state::RT_TAG };
	reg.emplace<scn::camera_component>(cam, cam_comp);

	scn::mouse_controller_component ctrl;
	ctrl.position       = m_camera_state.position;
	ctrl.rotation       = m_camera_state.rotation;
	ctrl.distance       = m_camera_state.distance;
	ctrl.movement_speed = m_camera_state.movement_speed;
	ctrl.rotating_speed = m_camera_state.rotating_speed;
	reg.emplace<scn::mouse_controller_component>(cam, ctrl);

	glm::mat4 orientation = glm::toMat4(glm::quat(ctrl.rotation));
	glm::mat4 cam_matrix = glm::translate(glm::mat4(1.0f), ctrl.position)
		* orientation
		* glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, ctrl.distance));
	reg.emplace<scn::local_transform>(cam, scn::local_transform{ cam_matrix });
	reg.emplace<scn::world_transform>(cam);
	reg.emplace<scn::name_component>(cam, "Editor Camera");
	reg.emplace<scn::depth_level>(cam);
}

// ─── Helpers ────────────────────────────────────────────────────────────────

void edt::editor_system::clear_selection()
{
	m_hierarchy_panel->set_selected_node(nullptr);
	m_inspector_panel->set_selected_node(nullptr);
}

void edt::editor_system::sync_viewport_selection()
{
	auto* selected = m_hierarchy_panel->get_selected_node();
	if (!selected) {
		m_viewport_panel->set_selected_entity(entt::null);
		return;
	}

	auto lvl_mgr = m_state.lvl_manager.lock();
	if (!lvl_mgr || m_state.active_world_idx >= (int)m_state.world_names.size()) {
		m_viewport_panel->set_selected_entity(entt::null);
		return;
	}

	auto& reg = lvl_mgr->get_level().get_world(m_state.world_names[m_state.active_world_idx]).state();
	ecs::entity found = entt::null;
	reg.view<scn::name_component>().each([&](auto ent, const scn::name_component& nc) {
		if (nc.name == selected->name) found = ent;
	});
	m_viewport_panel->set_selected_entity(found);
}
