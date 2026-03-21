#include "edt_init_helpers.h"
#include "edt_editor_system.h"
#include "edt_editor_layer.h"
#include "edt_scene_hierarchy_panel.h"
#include "edt_inspector_panel.h"
#include "edt_viewport_panel.h"
#include "edt_console_panel.h"
#include "edt_asset_browser_panel.h"
#include "edt_component_ui_registry.h"
#include "edt_component_renderers.h"
#include "edt_input_manager.h"
#include "edt_editor_camera.h"
#include "edt_shared_state.h"
#include "edt_scene_editor.h"
#include "edt_level_controller.h"
#include "edt_world_controller.h"
#include "edt_file_dialog.h"
#include "res_system.h"
#include "desc_system.h"
#include "rnd_render_system.h"
#include "gui_system.h"
#include "inp_ecs_input_manager.h"
#include "scn_camera_component.hpp"
#include "scn_camera_controller_component.hpp"
#include "eng_transform_3d.hpp"
#include "level/scn_level_manager.h"
#include "level/scn_level.h"
#include "level/scn_world.h"
#include "engine_log.h"
#include <entt/entt.hpp>
#include <glm/gtx/quaternion.hpp>

namespace edt::init_helpers {

void setup_panels(
	std::shared_ptr<scene_hierarchy_panel>& hierarchy,
	std::shared_ptr<inspector_panel>& inspector,
	std::shared_ptr<viewport_panel>& viewport,
	std::shared_ptr<console_panel>& console,
	std::shared_ptr<asset_browser_panel>& asset_browser,
	component_ui_registry& registry,
	editor_system& sys)
{
	hierarchy = std::make_shared<scene_hierarchy_panel>();
	inspector = std::make_shared<inspector_panel>();
	register_desc_component_renderers(registry);
	inspector->set_component_ui_registry(&registry);
	console = std::make_shared<console_panel>();
	asset_browser = std::make_shared<asset_browser_panel>();
	asset_browser->set_base_path(res::resource_system::get_resources_path());

	auto console_weak = std::weak_ptr<console_panel>(console);
	engine::set_log_callback([console_weak](std::string_view category, engine::log_level level, std::string_view message) {
		auto panel = console_weak.lock();
		if (!panel) return;
		edt::log_level edt_level = edt::log_level::info;
		if (level == engine::log_level::warn)  edt_level = edt::log_level::warning;
		if (level == engine::log_level::error) edt_level = edt::log_level::error;
		panel->add_log(edt_level, std::format("[{}] {}", category, message));
	});
}

void setup_viewport_panel(
	std::shared_ptr<viewport_panel>& viewport,
	std::shared_ptr<input_manager>& input,
	std::shared_ptr<inp::ecs_input_manager>& ecs_input,
	editor_system& sys,
	shared_state& state,
	rnd::render_system& rnd,
	gui::gui_system& gui,
	res::resource_system& res)
{
	viewport = std::make_shared<viewport_panel>(rnd, gui);
	viewport->set_registry_getter([&state]() -> entt::registry* {
		auto lvl_mgr = state.lvl_manager.lock();
		if (!lvl_mgr || state.active_world_idx >= (int)state.world_names.size()) return nullptr;
		const auto& name = state.world_names[state.active_world_idx];
		auto& level = lvl_mgr->get_level();
		if (!level.has_world(name)) return nullptr;
		return &level.get_world(name).state();
	});
	viewport->set_input_manager(input);
	viewport->set_ecs_input_manager(ecs_input);
	viewport->set_on_asset_dropped([&sys](const std::string& abs_path) {
		if (!sys.m_state.editor_tag.is_valid()) return;
		auto base = res::resource_system::get_resources_path();
		auto rel  = std::filesystem::path(abs_path).lexically_relative(base);
		res::tag tag = res::tag::make(rel.string());
		const std::string stem = rel.stem().string();
		sys.m_pending_desc_handle = sys.m_res.require<desc::desc_base>(tag);
		sys.m_pending_desc_tag = tag;
		sys.m_pending_desc_key = sys.m_scene_editor.make_unique_key(stem.empty() ? "Asset" : stem);
		sys.editor_layer->register_implicit("edt/drop_loading", [&sys] { return sys.poll_pending_desc(); });
	});
}

void wire_hierarchy_callbacks(
	scene_hierarchy_panel& hierarchy,
	inspector_panel& inspector,
	viewport_panel& viewport,
	scene_editor& scene_ed,
	world_controller& world_ctrl,
	shared_state& state)
{
	hierarchy.set_on_node_selected([&](scn::prefab_desc::prefab_node* node) {
		inspector.set_selected_node(node);
		auto lvl_mgr = state.lvl_manager.lock();
		if (lvl_mgr && node && state.active_world_idx < (int)state.world_names.size()) {
			auto& reg = lvl_mgr->get_level().get_world(state.world_names[state.active_world_idx]).state();
			ecs::entity found = entt::null;
			reg.view<scn::name_component>().each([&](auto ent, const scn::name_component& nc) {
				if (nc.name == node->name) found = ent;
			});
			viewport.set_selected_entity(found);
		} else {
			viewport.set_selected_entity(entt::null);
		}
	});

	hierarchy.set_on_world_changed([&](int idx) {
		world_ctrl.switch_to_world(idx);
	});
	hierarchy.set_on_create_world([&](const std::string& name) {
		world_ctrl.create_world(name);
	});
	hierarchy.set_on_create_node([&](const std::string& type_name) {
		scene_ed.create_entity(type_name);
	});
	hierarchy.set_on_delete_node([&](const std::string& name) {
		scene_ed.delete_entity(name);
	});
	hierarchy.set_on_delete_nodes([&](const std::vector<std::string>& names) {
		scene_ed.delete_entities(names);
	});
	hierarchy.set_on_rename_node([&](const std::string& /*old_name*/, const std::string& /*new_name*/) {
		state.is_dirty = true;
		scene_ed.serialize_and_push();
	});
	hierarchy.set_on_duplicate_node([&](const std::string& name) {
		scene_ed.duplicate_entity(name);
	});
	hierarchy.set_on_duplicate_nodes([&](const std::vector<std::string>& names) {
		scene_ed.duplicate_entities(names);
	});
	hierarchy.set_on_reparent_node([&](const std::string& node_name, const std::string& new_parent_name, int insert_index) {
		scene_ed.reparent_entity(node_name, new_parent_name, insert_index);
	});

	inspector.set_on_node_changed([&]() {
		state.is_dirty = true;
		scene_ed.serialize_and_push();
	});

	viewport.set_on_transform_committed(
		[&](const std::string& name, glm::vec3 pos, glm::vec3 rot, glm::vec3 scale) {
			scene_ed.on_transform_committed(name, pos, rot, scale);
		});
}

void wire_dockspace_menus(
	editor_layer& layer,
	level_controller& level_ctrl,
	editor_system& sys,
	shared_state& state,
	file_dialog& dlg,
	std::function<void()>& exit_action)
{
	auto& ds = layer.get_dockspace();
	ds.set_on_new_level([&] {
		if (state.is_dirty) {
			level_ctrl.m_confirm_action = [&] { level_ctrl.new_level(); };
			layer.register_implicit("edt/new_confirm", [&] {
				return level_ctrl.show_exit_confirm();
			});
		} else {
			level_ctrl.new_level();
		}
	});
	ds.set_on_open_level([&] {
		dlg.set_current_path(res::resource_system::get_resources_path());
		dlg.set_select_mode(edt::file_dialog::SELECT_MODE::FILES_ONLY);
		dlg.clear_extension_filters();
		dlg.add_extension_filter(".desc");
		layer.register_implicit("edt/load_level", [&sys] { return sys.load_level(); });
	});
	ds.set_on_save_level([&] {
		layer.register_implicit("edt/save_level", [&level_ctrl] { return level_ctrl.save_level(); });
	});
	ds.set_on_quick_save([&] {
		if (state.level_tag.is_valid() && state.editor_tag.is_valid()) {
			level_ctrl.quick_save_level();
		} else if (state.editor_tag.is_valid()) {
			layer.register_implicit("edt/save_level", [&level_ctrl] { return level_ctrl.save_level(); });
		}
	});
	ds.set_on_import_model([&] {
		dlg.set_current_path(std::filesystem::current_path());
		dlg.set_select_mode(edt::file_dialog::SELECT_MODE::FILES_ONLY);
		dlg.clear_extension_filters();
		dlg.add_extension_filter(".glb");
		dlg.add_extension_filter(".obj");
		dlg.add_extension_filter(".fbx");
		dlg.add_extension_filter(".gltf");
		layer.register_implicit("edt/import_model", [&sys] { return sys.show_file_dialog(); });
	});
	ds.set_on_exit([&] {
		if (state.is_dirty) {
			level_ctrl.m_confirm_action = exit_action;
			layer.register_implicit("edt/exit_confirm", [&level_ctrl] {
				return level_ctrl.show_exit_confirm();
			});
		} else {
			exit_action();
		}
	});
}

void wire_controller_callbacks(
	editor_system& sys,
	scene_editor& scene_ed,
	world_controller& world_ctrl,
	level_controller& level_ctrl,
	editor_layer& layer,
	scene_hierarchy_panel& hierarchy,
	inspector_panel& inspector,
	viewport_panel& viewport,
	shared_state& state)
{
	scene_ed.on_before_modify = [&hierarchy, &inspector] {
		hierarchy.set_selected_node(nullptr);
		inspector.set_selected_node(nullptr);
	};
	scene_ed.on_after_push = [&hierarchy, &viewport, &state] {
		auto* selected = hierarchy.get_selected_node();
		if (!selected) {
			viewport.set_selected_entity(entt::null);
			return;
		}
		auto lvl_mgr = state.lvl_manager.lock();
		if (!lvl_mgr || state.active_world_idx >= (int)state.world_names.size()) {
			viewport.set_selected_entity(entt::null);
			return;
		}
		auto& reg = lvl_mgr->get_level().get_world(state.world_names[state.active_world_idx]).state();
		ecs::entity found = entt::null;
		reg.view<scn::name_component>().each([&](auto ent, const scn::name_component& nc) {
			if (nc.name == selected->name) found = ent;
		});
		viewport.set_selected_entity(found);
	};
	scene_ed.on_save_camera = [&sys](entt::registry& reg) { sys.save_editor_camera_state(reg); };
	scene_ed.on_inject_camera = [&sys](entt::registry& reg) { sys.inject_editor_camera(reg); };

	world_ctrl.on_save_camera = [&sys](entt::registry& reg) { sys.save_editor_camera_state(reg); };
	world_ctrl.on_inject_camera = [&sys](entt::registry& reg) { sys.inject_editor_camera(reg); };
	world_ctrl.on_world_switched = [&hierarchy, &inspector, &viewport, &state](int idx) {
		hierarchy.set_world_desc(&state.active_world_desc());
		hierarchy.set_world_names(state.world_names, idx);
		inspector.set_selected_node(nullptr);
		hierarchy.set_selected_node(nullptr);
		viewport.set_selected_entity(entt::null);
	};

	level_ctrl.on_editor_world_reloaded = [] {
		egLOG("Editor", "Editor world reloaded");
	};
	level_ctrl.on_need_save_dialog = [&layer, &level_ctrl] {
		layer.register_implicit("edt/save_level", [&level_ctrl] { return level_ctrl.save_level(); });
	};
}

void save_camera_state(entt::registry& reg, editor_camera_state& state)
{
	auto view = reg.view<edt::editor_camera_tag, scn::mouse_controller_component>();
	for (auto ent : view) {
		auto& ctrl = reg.get<scn::mouse_controller_component>(ent);
		state.position = ctrl.position;
		state.rotation = ctrl.rotation;
		state.distance = ctrl.distance;
		break;
	}
}

void inject_camera(entt::registry& reg, const editor_camera_state& state)
{
	for (auto ent : reg.view<edt::editor_camera_tag>())
		reg.destroy(ent);

	entt::entity cam = reg.create();
	reg.emplace<edt::editor_camera_tag>(cam);

	scn::camera_component cam_comp;
	cam_comp.fov           = state.fov;
	cam_comp.near_distance = state.near_distance;
	cam_comp.far_distance  = state.far_distance;
	cam_comp.texture       = res::tag{ edt::editor_camera_state::RT_TAG };
	reg.emplace<scn::camera_component>(cam, cam_comp);

	scn::mouse_controller_component ctrl;
	ctrl.position       = state.position;
	ctrl.rotation       = state.rotation;
	ctrl.distance       = state.distance;
	ctrl.movement_speed = state.movement_speed;
	ctrl.rotating_speed = state.rotating_speed;
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

} // namespace edt::init_helpers
