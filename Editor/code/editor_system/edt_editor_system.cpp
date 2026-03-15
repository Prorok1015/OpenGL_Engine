#include "edt_editor_system.h"
#include "edt_scene_serializer.hpp"
#include "gs_game_system.h"
#include "res_system.h"
#include "level/scn_level_desc.h"
#include "rnd_render_system.h"
#include "scn_primitives.h"
#include "scn_camera_component.hpp"
#include "scn_camera_controller_component.hpp"
#include "ecs_event.hpp"
#include "eng_transform_3d.hpp"
#include "inp_input_system.h"
#include "edt_input_manager.h"
#include "edt_guizmo.hpp"
#include "gui_system.h"
#include <imgui.h>
#include "scn_material_desc.h"
#include "scn_skinning_prototype_desc.h"

#include "ds/ds_svg_writer.hpp"
#include "level/scn_world.h"
#include "level/scn_level.h"
#include "level/scn_level_manager.h"

#include "edt_spawn_system.h"
#include "edt_component_renderers.hpp"
#include <entt/entt.hpp>
#include <level/scn_prefab_desc.h>
#include <fstream>
#include <boost/json.hpp>

edt::editor_system::editor_system(desc::desc_system& desc_system_)
	: desc_system(desc_system_)
{
	input = std::make_shared<edt::input_manager>();

	file_dialog.set_current_path(res::get_system().get_resources_path());

	auto txt = rnd::get_system().get_texture_manager().generate_texture(res::tag(res::tag::memory, "__black"), {1,1}, rnd::driver::texture_header::TYPE::RGB8, {0, 0, 0});
	auto txt2 = rnd::get_system().get_texture_manager().generate_texture(res::tag(res::tag::memory, "__red"), {1,1}, rnd::driver::texture_header::TYPE::RGB8, {255, 0, 0});
}

edt::editor_system::~editor_system()
{
}

void edt::editor_system::init(ds::app_data_storage& data)
{
	editor_layer = std::make_shared<edt::editor_layer>();

	auto& gui_system = data.require<gui::gui_system>();
	gui_system.push_layer(editor_layer);

	editor_layer->register_tool("File/Import...", [this] { return show_file_dialog(); });
	editor_layer->register_tool("Window/Toolbar", [this] { return show_toolbar(); });
	editor_layer->register_tool("Window/Scene", [this] { return show_scene(); });
	editor_layer->register_tool("Editor/Clear", [this] { return show_clear_cache(); });


	m_lvl_manager = data.require_shared<scn::level_manager>();
	auto& sfactory = data.require<ecs::system_factory>();
	sfactory.register_automatic_system<edt::spawn_system>("edt::spawn_system");

	auto& rndsys = data.require<rnd::render_system>();


	auto& inp_sys = data.require<inp::input_system>();
	inp_sys.push_input_layer(inp_sys.get_focused_window(), inp::input_layer{ input, true });
	ecs_input = data.require_shared<inp::ecs_input_manager>();

	// Создаём панели и добавляем в panel_manager
	m_hierarchy_panel    = std::make_shared<scene_hierarchy_panel>();
	m_inspector_panel    = std::make_shared<inspector_panel>();
	register_builtin_component_renderers(*m_inspector_panel);
	m_viewport_panel     = std::make_shared<viewport_panel>();
	m_console_panel      = std::make_shared<console_panel>();
	m_asset_browser_panel = std::make_shared<asset_browser_panel>();

	// Pass the resource base path so the browser can list files
	m_asset_browser_panel->set_base_path(res::resource_system::get_resources_path());


	m_viewport_panel->set_input_manager(input);
	m_viewport_panel->set_ecs_input_manager(ecs_input);
	m_viewport_panel->set_on_asset_dropped([this](const std::string& abs_path) {
		if (!registry_sp)
			return;
		auto base = res::resource_system::get_resources_path();
		auto rel  = std::filesystem::path(abs_path).lexically_relative(base);
		res::tag tag = res::tag::make(rel.string());
		auto handle = res::get_system().require_sync<scn::skinning_prototype_desc>(tag);
		if (handle.is_ready()) {
			auto anchors = registry_sp->view<scn::scene_anchor_component>();
			if (!anchors.empty())
				handle->load_prototype(*registry_sp, anchors.front());
		} else if (handle.has_error()) {
			egLOG("Editor/Viewport", "Failed to load dropped asset: {}", handle.get_control_block()->error_msg);
		}
	});

	m_hierarchy_panel->set_on_entity_selected([this](ecs::entity ent) {
		selected_entity = ent;
		m_inspector_panel->set_selected_entity(ent);
		m_viewport_panel->set_selected_entity(ent);
	});

	auto& pm = editor_layer->get_panel_manager();
	pm.add_panel(m_hierarchy_panel);
	pm.add_panel(m_inspector_panel);
	pm.add_panel(m_viewport_panel);
	pm.add_panel(m_console_panel);
	pm.add_panel(m_asset_browser_panel);

	auto& ds = editor_layer->get_dockspace();
	ds.set_on_new_level([this]  { new_level(); });
	ds.set_on_open_level([this] {
		editor_layer->register_implicit("edt/load_level", [this] { return load_level(); });
	});
	ds.set_on_save_level([this] {
		editor_layer->register_implicit("edt/save_level", [this] { return save_level(); });
	});
}

void mark_node_for_animation_stop(entt::registry* registry_sp, entt::entity ent)
{
	if (registry_sp->all_of<scn::keyframes_component>(ent)) {
		registry_sp->remove<scn::playable_animation_component>(ent);
	}

	if (auto* children = registry_sp->try_get<scn::children_component>(ent)) {
		for (auto& child : children->children) {
			mark_node_for_animation_stop(registry_sp, child);
		}
	}
}

void mark_node_for_animation(entt::registry* registry_sp, entt::entity ent, const scn::animation& animation)
{
	if (registry_sp->all_of<scn::keyframes_component>(ent)) {
		scn::playable_animation_component tmp{ animation.name, animation.duration, animation.ticks_per_second };
		registry_sp->emplace_or_replace<scn::playable_animation_component>(ent, std::move(tmp));
	}

	if (auto* children = registry_sp->try_get<scn::children_component>(ent)) {
		for (auto& child : children->children) {
			mark_node_for_animation(registry_sp, child, animation);
		}
	}
}

void edt::editor_system::show_tree_items(ecs::entity ent)
{
	std::string obj_idx = std::to_string((int)ent);
	std::string name = "Node##" + obj_idx;
	if (registry_sp->all_of<scn::name_component>(ent)) {
		auto& com_name = registry_sp->get<scn::name_component>(ent);
		if (!com_name.name.empty()) {
			name = (registry_sp->all_of<scn::mesh_component>(ent) ? "[MESH] " : "") + com_name.name + "##" + obj_idx;
		}
	}
	static ecs::entity selected_node = entt::null;
	static ecs::entity parent_node_to_add = entt::null;
	static char buf[256];

	if (ImGui::TreeNode(name.c_str()))
	{
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			ImGui::OpenPopup("tree_context_menu");
			selected_node = ent;
		}

		if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
			selected_entity = ent;
		}

		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {

			if (registry_sp->all_of<scn::name_component>(ent)) {
				auto& name_comp = registry_sp->get<scn::name_component>(ent);
				strncpy(buf, name_comp.name.c_str(), sizeof(buf) - 1);
				buf[sizeof(buf) - 1] = '\0';
				ImGui::OpenPopup("Rename Node");
			}
		}

		if (ImGui::BeginPopup("Rename Node")) {
			if (ImGui::InputText("##rename", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
				if (registry_sp->all_of<scn::name_component>(ent)) {
					auto& name_comp = registry_sp->get<scn::name_component>(ent);
					name_comp.name = buf;
				}

				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		if (parent_node_to_add != entt::null) {
			if (ImGui::BeginPopup("create_new_entity")) {
				if (ImGui::InputText("##new_entity_name", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
					ecs::entity child = registry_sp->create();
					registry_sp->emplace<scn::name_component>(child, buf);
					registry_sp->emplace<scn::parent_component>(child, parent_node_to_add);
					if (registry_sp->all_of<scn::children_component>(parent_node_to_add)) {
						auto& children = registry_sp->get<scn::children_component>(parent_node_to_add);
						auto children_list = children.children;
						children_list.push_back(child);
						registry_sp->remove<scn::children_component>(parent_node_to_add);
						registry_sp->emplace<scn::children_component>(parent_node_to_add, children_list);
					}
					else {
						registry_sp->emplace<scn::children_component>(parent_node_to_add, std::vector<ecs::entity>{child});
					}

					parent_node_to_add = entt::null;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		}

		if (selected_node == ent) {
			if (ImGui::BeginPopup("tree_context_menu")) {
				if (ImGui::MenuItem("Add Camera")) {
					registry_sp->emplace<scn::camera_component>(ent,
						scn::camera_component{ .m_viewport = glm::ivec4{100,100, 500, 500} }
					);
				}

				if (ImGui::MenuItem("Add Directional Light")) {
					registry_sp->emplace<scn::directional_light>(ent,
						glm::vec4(-0.2f, -1.0f, -0.3f, 0.0),
						glm::vec4(0.5f, 0.5f, 0.5f, 1.0),
						glm::vec4(0.2f, 0.2f, 0.2f, 1.0),
						glm::vec4(1.0f)
					);
				}
				if (registry_sp->all_of<scn::renderable>(ent)) {
					if (ImGui::MenuItem("Remove Render Flag")) {
						registry_sp->remove<scn::renderable>(ent);
					}
				} else {
					if (ImGui::MenuItem("Add Render Flag")) {
						registry_sp->emplace<scn::renderable>(ent);
					}
				}
				if (ImGui::MenuItem("Add Child Entity")) {
					strncpy(buf, "New Entity", sizeof(buf) - 1);
					buf[sizeof(buf) - 1] = '\0';
					parent_node_to_add = selected_node;
				}
				ImGui::EndPopup();

				if (parent_node_to_add == selected_node) {
					ImGui::OpenPopup("create_new_entity");
				}
			}
		}

		struct edt_playable_animation {
			std::string name;
			int idx = -1;
		};

		if (registry_sp->all_of<scn::animations_component>(ent)) {
			auto& anims = registry_sp->get<scn::animations_component>(ent);
			std::string_view play_anim_name;

			if (registry_sp->all_of<edt_playable_animation>(ent)) {
				auto& play = registry_sp->get<edt_playable_animation>(ent);
				play_anim_name = play.name;
			}

			std::vector<std::string> names;
			names.push_back("NONE");
			int cur = 0, i = 0;
			for (auto& anim : anims.animations)
			{
				if (play_anim_name == anim.name && cur == 0) {
					cur = i + 1;
				}
				names.push_back(anim.name);
				i++;
			}

			if (ImGui::BeginCombo(("Animations##" + obj_idx).c_str(), names[cur].c_str(), 0))
			{
				for (int n = 0; n < names.size(); ++n)
				{
					const bool is_selected = (cur == n);
					if (ImGui::Selectable(names[n].c_str(), is_selected)) {
						int old = cur;
						cur = n;

						if (cur == 0) {
							if (registry_sp->all_of<edt_playable_animation>(ent)) {
								registry_sp->remove<edt_playable_animation>(ent);
								mark_node_for_animation_stop(registry_sp.get(), ent);
							}
						} else {
							registry_sp->emplace_or_replace<edt_playable_animation>(ent, edt_playable_animation{ .name = names[cur], .idx = cur });
							if (old != cur) {
								mark_node_for_animation(registry_sp.get(), ent, anims.animations[cur - 1]);
							}
						}
					}

					// Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}

		if (registry_sp->all_of<scn::local_transform>(ent)) {
			auto& trans = registry_sp->get<scn::local_transform>(ent);
			bool is_update = false;
			eng::transform3d tr{ trans.local };

			glm::vec3 scale = tr.get_scale();
			std::string scale_name = "scale##" + obj_idx;
			ImGui::DragFloat3(scale_name.c_str(), glm::value_ptr(scale), 0.01f, 0.1f, 100.0f, "%.01f");
			if (scale != tr.get_scale()) {
				tr.set_scale(scale);
				is_update = true;
			}

			glm::vec3 translate = tr.get_pos();
			std::string translate_name = "position##" + obj_idx;
			ImGui::DragFloat3(translate_name.c_str(), glm::value_ptr(translate), 0.01f, -10000.0f, 10000.0f, "%.01f");
			if (translate != tr.get_pos()) {
				tr.set_pos(translate);
				is_update = true;
			}

			glm::vec3 orientation = tr.get_angles_degrees();
			glm::vec3 original_d = tr.get_angles_degrees();
			glm::quat original = tr.get_rotation();
			std::string orientation_name = "rotation##" + obj_idx;
			ImGui::DragFloat3(orientation_name.c_str(), glm::value_ptr(orientation));
			glm::quat result_orientation = glm::quat(glm::radians(orientation));
			if (std::abs(glm::dot(result_orientation, original)) < 0.999f) {
				tr.set_rotation(result_orientation);
				is_update = true;
			}

			if (is_update) {
				trans.local = tr.to_matrix();
				registry_sp->ctx().get<ecs::event<scn::transform_updated>>().emit(ent);
			}
		}

		if (registry_sp->all_of<scn::directional_light>(ent)) {
			auto& color = registry_sp->get<scn::directional_light>(ent);
			ImGui::ColorEdit3("Light Color", glm::value_ptr(color.diffuse));
			ImGui::ColorEdit3("Ambient Color", glm::value_ptr(color.ambient));
			ImGui::ColorEdit3("Specular Color", glm::value_ptr(color.specular));
			ImGui::DragFloat3("Position", glm::value_ptr(color.direction), 0.01f, -1.0f, 1.0f, "%.1f");
		}

		if (registry_sp->all_of<scn::camera_component>(ent)) {
			auto& camera = registry_sp->get<scn::camera_component>(ent);
			ImGui::Text("fov: %3.f", camera.fov);
			ImGui::Text("near: %3.f", camera.near_distance);
			ImGui::Text("far: %3.f", camera.far_distance);
			ImGui::Text("x: %d, y: %d, width: %d, height: %d", camera.m_viewport.center.x, camera.m_viewport.center.y, camera.m_viewport.size.x, camera.m_viewport.size.y);
		}

		if (registry_sp->all_of<scn::children_component>(ent)) {
			auto& children = registry_sp->get<scn::children_component>(ent);
			for (auto& child : children.children)
			{
				show_tree_items(child);
			}
		}

		ImGui::TreePop();
		ImGui::Spacing();
	}
}

bool edt::editor_system::show_toolbar()
{
	ImGuiIO& io = ImGui::GetIO();

	static bool is_last_frame_loaded = false;
	if (is_last_frame_loaded)
	{
		static std::once_flag f;
		std::call_once(f, [&]() {
			backpackent = backpack->load_prototype(*registry_sp, world_anchor);
			auto geom = backpack->geometry;
			rtree = geom->rtree;

			ds::svg_writer query_svg("rtree_query_visualization.svg", 2048, 2048);
			for (auto& v : rtree.data) {
				query_svg.add_rect(v.box, v.is_leaf() ? "green" : "red");
			}
		});
	}

	bool is_open = true;
	if (ImGui::Begin("Observer", &is_open))
	{
		ImGui::Text("Common");
		ImGui::Separator();
		ImGui::ColorEdit4("Clear color", glm::value_ptr(clear_color));
		rnd::get_system().get_driver()->set_clear_color(clear_color);
		ImGui::Separator();
		ImGui::NewLine();

		ImGui::Text("Shader");
		ImGui::Separator();

		if (ImGui::Button("Reload Shaders")) {
			gs::get_system().reload_shaders();
		}

		if (ImGui::Button("Reload Texture")) {
			rnd::get_system().get_texture_manager().clear_cache();
		}
		ImGui::Separator();
		ImGui::NewLine();

		if (ImGui::CollapsingHeader("Scene Objects"))
		{
			if (registry_sp) {
				for (const auto ent : registry_sp->view<scn::scene_anchor_component>()) {
					show_tree_items(ent);
				}
			}
		}

		ImGui::Separator();
		ImGui::NewLine();
		if (registry_sp)
		{
			ImGui::Text("Camera");
			ImGui::Separator();
			std::vector<std::string> names;
			std::vector<ecs::entity> cameras;
			int cam_id = 1;
			int cam_cur = 0;
			int cam_cur_id = -1;
			auto cameras_view = registry_sp->view<scn::camera_component>();
			for (auto& ent : cameras_view)
			{
				cameras.push_back(ent);
				if (registry_sp->all_of<scn::name_component>(ent)) {
					auto& name = registry_sp->get<scn::name_component>(ent);
					names.push_back(name.name);
				} else {
					names.push_back("Camera" + std::to_string(cam_id++));
				}

				if (registry_sp->all_of<scn::renderable>(ent)) {
					cam_cur_id = cam_cur;
				}

				++cam_cur;
			}

			if (cam_cur_id != -1)
				if (ImGui::BeginCombo("Current Camera", names[cam_cur_id].c_str(), 0))
			{
				for (int n = 0; n < names.size(); ++n)
				{
					const bool is_selected = (cam_cur_id == n);
					if (ImGui::Selectable(names[n].c_str(), is_selected)) {
						auto old = cameras[cam_cur_id];
						auto new_one = cameras[n];
						cam_cur_id = n;
						registry_sp->emplace<scn::renderable>(new_one);
						registry_sp->remove<scn::renderable>(old);
					}

					// Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}

			for (const auto ent : registry_sp->view<scn::camera_component, scn::renderable>()) {
				eng::transform3d ct{ glm::mat4{1.0} };
				if (registry_sp->all_of<scn::local_transform>(ent)) {
					auto& trans = registry_sp->get<scn::local_transform>(ent);
					ct = eng::transform3d{ trans.local };
				}
				ImGui::Text("pitch: %.3f, yaw: %.3f, roll: %.3f", glm::degrees(ct.get_pitch()), glm::degrees(ct.get_yaw()), glm::degrees(ct.get_roll()));
				auto pos = ct.get_pos();
				ImGui::Text("x: %.3f, y: %.3f, z: %.3f", pos.x, pos.y, pos.z);
			}
		}

		ImGui::Text("Scene");
		ImGui::Separator();

		ImGui::Text("Import model");
		static int selected_model_idx = 0;
		if (!imported_models_list.empty()) {
			std::string buf = std::string{ imported_models_list[selected_model_idx].view() };
			if (ImGui::BeginCombo("##imported_objects", buf.c_str(), 0))
			{
				for (int n = 0; n < imported_models_list.size(); ++n)
				{
					const bool is_selected = (selected_model_idx == n);
					buf = std::string{ imported_models_list[n].view() };
					if (ImGui::Selectable(buf.c_str(), is_selected)) {
						selected_model_idx = n;
					}

					// Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
		}

		if (registry_sp && ImGui::Button("Create object on scene")) {
			auto robot = res::get_system().require_sync<scn::skinning_prototype_desc>(imported_models_list[selected_model_idx]);
			if (robot.is_ready()) {
				auto anchors = registry_sp->view<scn::scene_anchor_component>();
				if (!anchors.empty())
					robot->load_prototype(*registry_sp, anchors.front());
			}
			if (robot.has_error()) {
				egLOG("Editor/Observer", "Error loading model prototype: {}", robot.get_control_block()->error_msg);
			}
		}

		ImGui::Separator();
		if (ImGui::BeginCombo("Render mode", render_modes_list[current_render_mode].c_str(), 0))
		{
			for (int n = 0; n < render_modes_list.size(); n++)
			{
				const bool is_selected = (current_render_mode == n);
				if (ImGui::Selectable(render_modes_list[n].c_str(), is_selected)) {
					current_render_mode = n;
					rnd::get_system().set_render_mode(mmap[render_modes_list[n]]);
				}

				// Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
				if (is_selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		eng::transform3d ct;
		ImGui::Text("pitch: %.3f, yaw: %.3f, roll: %.3f", glm::degrees(ct.get_pitch()), glm::degrees(ct.get_yaw()), glm::degrees(ct.get_roll()));
		auto pos = ct.get_pos();
		ImGui::Text("x: %.3f, y: %.3f, z: %.3f", pos.x, pos.y, pos.z);

		static bool edit_mode_enabled = false; // Variable to track the editing mode state

		// Check if a valid entity is selected
		if (edit_mode_enabled || selected_entity != entt::null) {
			if (ImGui::Button(edit_mode_enabled ? "Disable Object Edit Mode" : "Enable Object Edit Mode")) {
				edit_mode_enabled = !edit_mode_enabled; // Toggle the editing mode state
				if (edit_mode_enabled) {
					registry_sp->emplace<scn::mouse_controller_component>(selected_entity);
					// Remove mouse controller from the camera if it exists
					for (const auto ent : registry_sp->view<scn::camera_component>()) {
						if (registry_sp->all_of<scn::mouse_controller_component>(ent)) {
							registry_sp->remove<scn::mouse_controller_component>(ent);
						}
					}
				} else {
					registry_sp->remove<scn::mouse_controller_component>(selected_entity);
					// Re-add mouse controller to the camera if it was removed
					for (const auto ent : registry_sp->view<scn::camera_component>()) {
						registry_sp->emplace<scn::mouse_controller_component>(ent);
					}
				}
			}
		} else {
			ImGui::BeginDisabled(); // Disable the button if no valid entity is selected and edit mode is not active
			ImGui::Button(edit_mode_enabled ? "Disable Object Edit Mode" : "Enable Object Edit Mode");
			ImGui::EndDisabled();
		}
	}
	ImGui::End();
	return is_open;
}

bool edt::editor_system::show_file_dialog()
{
	bool is_open = true;
	file_dialog.clear_extension_filters();
	file_dialog.add_extension_filter(".glb");
	file_dialog.add_extension_filter(".obj");
	file_dialog.add_extension_filter(".fbx");
	file_dialog.add_extension_filter(".gltf");

	if (file_dialog.show("Import", &is_open))
	{
		auto relateve = file_dialog.get_selected_path().lexically_relative(file_dialog.get_base_path());
		res::tag tag = res::tag::make(relateve.string());
		if (std::find(imported_models_list.begin(), imported_models_list.end(), tag) == imported_models_list.end()) {
			res::get_system().warmup<scn::skinning_prototype_desc>(tag);
			auto handle = res::get_system().require<scn::skinning_prototype_desc>(tag);
			handle.then([this, tag](auto& desc) {
				imported_models_list.push_back(desc.get_tag());
				imported_models_list.push_back(tag);
				});
		}
	}
    return is_open;
}

bool edt::editor_system::load_level()
{
	bool is_open = false;
	if (auto level_manager = m_lvl_manager.lock())
	{
		is_open = true;
		file_dialog.clear_extension_filters();
		file_dialog.add_extension_filter(".desc");

		if (file_dialog.show("Load Level", &is_open)) {
			auto relateve = file_dialog.get_selected_path().lexically_relative(file_dialog.get_base_path());
			res::tag tag = res::tag::make(relateve.string());
			if (level_manager->load(tag)) {
				scn::level& lvl = level_manager->get_level();
				m_level_tag = lvl.get_tag();

				// Capture world name and systems list from the loaded descriptor
				auto level_res = res::get_system().require_sync<scn::level_desc>(m_level_tag);
				if (level_res.is_ready() && !level_res->get_worlds().empty()) {
					auto& wh = level_res->get_worlds()[0];
					if (wh.is_ready()) {
						m_world_name    = std::string(wh->get_name());
						m_world_systems = wh->get_systems();
					}
				}

				auto& world = lvl.get_world("3d_scene");
				registry_sp = std::shared_ptr<entt::registry>(&world.state(), [] (entt::registry*) {});

				m_hierarchy_panel->set_registry(registry_sp);
				m_inspector_panel->set_registry(registry_sp);
				m_viewport_panel->set_registry(registry_sp);
			}
		}
	}
	return is_open;
}

void edt::editor_system::new_level()
{
	auto reg = std::make_shared<entt::registry>();

	// Required context: transform event so inspector edits don't assert
	reg->ctx().emplace<ecs::event<scn::transform_updated>>();
	reg->ctx().emplace<ecs::event<scn::hierarchy_updated>>();

	// Scene anchor (root)
	entt::entity anchor = reg->create();
	reg->emplace<scn::scene_anchor_component>(anchor);
	reg->emplace<scn::name_component>(anchor, "World");
	reg->emplace<scn::local_transform>(anchor);
	reg->emplace<scn::world_transform>(anchor);
	reg->emplace<scn::children_component>(anchor, std::vector<entt::entity>{});
	reg->emplace<scn::depth_level>(anchor, scn::depth_level{0});

	auto add_child = [&](entt::entity child) {
		reg->emplace<scn::parent_component>(child, anchor);
		reg->get<scn::children_component>(anchor).children.push_back(child);
	};

	// Camera
	entt::entity cam = reg->create();
	reg->emplace<scn::name_component>(cam, "Camera");
	reg->emplace<scn::local_transform>(cam);
	reg->emplace<scn::world_transform>(cam);
	reg->emplace<scn::camera_component>(cam);
	reg->emplace<scn::renderable>(cam);
	reg->emplace<scn::depth_level>(cam, scn::depth_level{1});
	add_child(cam);

	// Directional light
	entt::entity light = reg->create();
	reg->emplace<scn::name_component>(light, "Directional Light");
	reg->emplace<scn::local_transform>(light);
	reg->emplace<scn::world_transform>(light);
	reg->emplace<scn::directional_light>(light,
		glm::vec4{-0.2f, -1.0f, -0.3f, 0.f},
		glm::vec4{0.8f, 0.8f, 0.8f, 1.f},
		glm::vec4{0.2f, 0.2f, 0.2f, 1.f},
		glm::vec4{1.f});
	reg->emplace<scn::depth_level>(light, scn::depth_level{1});
	add_child(light);

	// Reset level metadata
	m_level_tag     = res::tag{};
	m_world_name    = "3d_scene";
	m_world_systems = {
		"inp::update_input_state",
		"scn::update_camera_matrix_system",
		"scn::depth_system",
		"scn::transform_system"
	};

	registry_sp = std::move(reg);

	m_hierarchy_panel->set_registry(registry_sp);
	m_inspector_panel->set_registry(registry_sp);
	m_viewport_panel->set_registry(registry_sp);
}

bool edt::editor_system::save_level()
{
	if (!registry_sp) {
		egLOG("Editor/Save", "No scene loaded — nothing to save.");
		return false;
	}

	// First call: fill path buffer and open the modal
	if (!m_save_dialog_open) {
		m_save_dialog_open = true;
		std::string current = m_level_tag.is_valid() ? std::string(m_level_tag.relative()) : "levels/my_level.desc";
		strncpy(m_save_path_buf, current.c_str(), sizeof(m_save_path_buf) - 1);
		m_save_path_buf[sizeof(m_save_path_buf) - 1] = '\0';
		ImGui::OpenPopup("Save Level##edt_save");
	}

	bool is_open = true;
	if (ImGui::BeginPopupModal("Save Level##edt_save", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::Text("Path (relative to res://):");
		ImGui::SetNextItemWidth(480.f);
		ImGui::InputText("##sp", m_save_path_buf, sizeof(m_save_path_buf));

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		const float bw = 110.f;
		const float avail = ImGui::GetContentRegionAvail().x;
		ImGui::SetCursorPosX((avail - bw * 2.f - ImGui::GetStyle().ItemSpacing.x) * 0.5f);

		if (ImGui::Button("Save", {bw, 0.f})) {
			// Derive level name from filename stem
			std::filesystem::path rel_path(m_save_path_buf);
			std::string level_name = rel_path.stem().string();
			if (level_name.empty()) level_name = "level";

			std::string world_name = m_world_name.empty() ? "3d_scene" : m_world_name;

			// Serialize
			boost::json::object level_json = edt::serialize_level(
				*registry_sp, level_name, world_name, m_world_systems);

			// Write to disk
			std::filesystem::path abs_path =
				res::resource_system::get_resources_path() / rel_path;
			std::error_code ec;
			std::filesystem::create_directories(abs_path.parent_path(), ec);
			std::ofstream out(abs_path);
			if (out) {
				out << boost::json::serialize(level_json);
				egLOG("Editor/Save", "Level saved to {}", abs_path.string());
			} else {
				egLOG("Editor/Save", "Failed to open file for writing: {}", abs_path.string());
			}

			m_save_dialog_open = false;
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Cancel", {bw, 0.f})) {
			m_save_dialog_open = false;
			is_open = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	} else if (m_save_dialog_open) {
		// Popup dismissed externally (e.g. Escape key)
		m_save_dialog_open = false;
		is_open = false;
	}

	return is_open;
}

bool edt::editor_system::show_web()
{
	const bool cur_is_show = editor_layer->is_tool_checked("Editor/Draw web");
	if (!cur_is_show && cur_is_show != is_show_web) {
		registry_sp->remove<scn::renderable>(editor_web);
		//ecs::remove_component<scn::renderable>(sky);
	}
	else if (cur_is_show && cur_is_show != is_show_web){
		registry_sp->emplace<scn::renderable>(editor_web);
		//ecs::add_component(sky, scn::renderable{});
	}

	is_show_web = cur_is_show;
	return true;
}

bool edt::editor_system::show_scene()
{
	bool is_open = true;
	if (ImGui::Begin("Scene", &is_open))
	{
		ImVec2 contentRegionAvailable = ImGui::GetContentRegionAvail();
		ImVec2 pos = ImGui::GetCursorScreenPos();
		glm::mat4 viewMatrix{ 1.0 };
		glm::mat4 projMat{ 1.0 };
		if (registry_sp) {
			for (const auto ent : registry_sp->view<scn::camera_component>()) {
				auto& camera = registry_sp->get<scn::camera_component>(ent);
				camera.m_viewport.size = glm::ivec2(contentRegionAvailable.x, contentRegionAvailable.y);
				if (registry_sp->all_of<scn::local_transform>(ent)) {
					auto& trans = registry_sp->get<scn::local_transform>(ent);
					viewMatrix = glm::inverse(trans.local);
				}
				if (camera.m_viewport.size != glm::ivec2{ 0 }) {
					projMat = glm::perspective(glm::radians(camera.fov), (float)camera.m_viewport.size.x / (float)camera.m_viewport.size.y, camera.near_distance, camera.far_distance);
				}
			}
		}

		glm::vec2 start{ pos.x, pos.y };
		glm::vec2 size{ contentRegionAvailable.x, contentRegionAvailable.y };
		draw_gizmo(start, size, viewMatrix, projMat);
		draw_scene_image(start, size);
	}
	ImGui::End();

	return is_open;
}

bool edt::editor_system::show_clear_cache()
{
	bool is_open = true;
	if (ImGui::Begin("Caches", &is_open))
	{
		if (ImGui::Button("Reload Shaders")) {
			gs::get_system().reload_shaders();
		}

		if (ImGui::Button("Reload Texture")) {
			rnd::get_system().get_texture_manager().clear_cache();
		}
	}
	ImGui::End();
	return is_open;
}

void edt::editor_system::draw_gizmo(const glm::vec2& start, const glm::vec2& size, const glm::mat4& view, const glm::mat4& proj)
{
	glm::vec2 guizmo_size{ 120, 120 };
	glm::vec2 guizmo_start = start + size - guizmo_size;
	ImGui::SetNextWindowPos(ImVec2(guizmo_start.x, guizmo_start.y));
	if (ImGui::BeginChild("Guizmo", ImVec2(guizmo_size.x, guizmo_size.y), 0, ImGuiWindowFlags_NoScrollbar)) {
		ImVec2 pos = ImGui::GetCursorScreenPos();
		ImVec2 size = ImGui::GetContentRegionAvail();
		edt::imgui::set_view_area(pos.x, pos.y, size.y);
		edt::imgui::draw_gizmo(view, proj);
	}
	ImGui::EndChild();
}

void edt::editor_system::draw_scene_image(const glm::vec2& pos, const glm::vec2& contentRegionAvailable)
{
	auto texture = rnd::get_system().get_texture_manager().find(res::tag(res::tag::memory, "__color_scene_rt"));
	auto* backend = gui::get_system().get_backend_interface();

	ImGuiStyle& style = ImGui::GetStyle();
	ImVec4 original_button_color = style.Colors[ImGuiCol_Button];
	ImVec4 original_button_hovered_color = style.Colors[ImGuiCol_ButtonHovered];
	ImVec4 original_button_active_color = style.Colors[ImGuiCol_ButtonActive];
	ImVec2 original_padding = style.FramePadding;
	style.Colors[ImGuiCol_Button] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	style.FramePadding = { 0, 0 };

	ImGui::SetCursorScreenPos(ImVec2{ pos.x, pos.y });
	ImGui::ImageButton("##ViewScene", backend->get_imgui_texture_from_texture(texture), ImVec2{ contentRegionAvailable.x, contentRegionAvailable.y }, ImVec2(0, 1), ImVec2(1, 0));

	glm::ivec4 rect = { pos, pos + contentRegionAvailable };
	ecs_input->set_input_area(glm::zero<glm::ivec4>(), true);
	input->set_input_area(rect, true);

	style.Colors[ImGuiCol_Button] = original_button_color;
	style.Colors[ImGuiCol_ButtonHovered] = original_button_hovered_color;
	style.Colors[ImGuiCol_ButtonActive] = original_button_active_color;
	style.FramePadding = original_padding;
	// let input to go to game
	if (ImGui::IsItemHovered()) {
		ecs_input->set_input_area(rect);
	}
}
