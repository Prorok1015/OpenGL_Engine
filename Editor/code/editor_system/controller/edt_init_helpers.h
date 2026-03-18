#pragma once
#include <functional>
#include <memory>
#include <entt/entity/fwd.hpp>

namespace inp { class ecs_input_manager; }
namespace rnd { class render_system; }
namespace gui { class gui_system; }
namespace res { class resource_system; }

namespace edt {
	class editor_system;
	class input_manager;
	class file_dialog;
	class scene_hierarchy_panel;
	class inspector_panel;
	class viewport_panel;
	class console_panel;
	class asset_browser_panel;
	class component_ui_registry;
	class editor_layer;
	class scene_editor;
	class level_controller;
	class world_controller;
	struct editor_camera_state;
	struct shared_state;

	namespace init_helpers {

		// Panel creation and basic configuration
		void setup_panels(
			std::shared_ptr<scene_hierarchy_panel>& hierarchy,
			std::shared_ptr<inspector_panel>& inspector,
			std::shared_ptr<viewport_panel>& viewport,
			std::shared_ptr<console_panel>& console,
			std::shared_ptr<asset_browser_panel>& asset_browser,
			component_ui_registry& registry,
			editor_system& sys);

		// Setup viewport panel with all its dependencies
		void setup_viewport_panel(
			std::shared_ptr<viewport_panel>& viewport,
			std::shared_ptr<input_manager>& input,
			std::shared_ptr<inp::ecs_input_manager>& ecs_input,
			editor_system& sys,
			shared_state& state,
			rnd::render_system& rnd,
			gui::gui_system& gui,
			res::resource_system& res);

		// Wire all hierarchy panel callbacks
		void wire_hierarchy_callbacks(
			scene_hierarchy_panel& hierarchy,
			inspector_panel& inspector,
			viewport_panel& viewport,
			scene_editor& scene_ed,
			world_controller& world_ctrl,
			shared_state& state);

		// Wire dockspace menu actions
		void wire_dockspace_menus(
			editor_layer& layer,
			level_controller& level_ctrl,
			editor_system& sys,
			shared_state& state,
			file_dialog& dlg,
			std::function<void()>& exit_action);

		// Wire controller callbacks (scene_editor, world_ctrl, level_ctrl)
		void wire_controller_callbacks(
			editor_system& sys,
			scene_editor& scene_ed,
			world_controller& world_ctrl,
			level_controller& level_ctrl,
			editor_layer& layer,
			scene_hierarchy_panel& hierarchy,
			inspector_panel& inspector,
			viewport_panel& viewport,
			shared_state& state);

		// Editor camera ECS operations
		void save_camera_state(entt::registry& reg, editor_camera_state& state);
		void inject_camera(entt::registry& reg, const editor_camera_state& state);

	} // namespace init_helpers
} // namespace edt
