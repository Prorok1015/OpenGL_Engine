#include "edt_editor_system.h"
#include "gs_game_system.h"
#include "res_system.h"
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
#include <misc/cpp/imgui_stdlib.h>
#include <boost/json.hpp>
#include <imgui.h>
#include "geom/rnd_geometry_desc.h"
#include "scn_material_desc.h"
#include "scn_skinning_prototype_desc.h"
#include "resource/resources/res_resource_text.h"

#include "ds/ds_svg_writer.hpp"
#include "level/scn_world.h"
#include "level/scn_level.h"
#include "level/scn_level_manager.h"

#include "edt_spawn_system.h"
#include <entt/entt.hpp>
#include <level/scn_prefab_desc.h>

void pretty_print( std::ostream& os, json::value const& jv, std::string* indent = nullptr )
{
    std::string indent_;
    if(! indent)
        indent = &indent_;
    switch(jv.kind())
    {
    case json::kind::object:
    {
        os << "{\n";
        indent->append(4, ' ');
        auto const& obj = jv.get_object();
        if(! obj.empty())
        {
            auto it = obj.begin();
            for(;;)
            {
                os << *indent << json::serialize(it->key()) << " : ";
                pretty_print(os, it->value(), indent);
                if(++it == obj.end())
                    break;
                os << ",\n";
            }
        }
        os << "\n";
        indent->resize(indent->size() - 4);
        os << *indent << "}";
        break;
    }

    case json::kind::array:
    {
        os << "[\n";
        indent->append(4, ' ');
        auto const& arr = jv.get_array();
        if(! arr.empty())
        {
            auto it = arr.begin();
            for(;;)
            {
                os << *indent;
                pretty_print( os, *it, indent);
                if(++it == arr.end())
                    break;
                os << ",\n";
            }
        }
        os << "\n";
        indent->resize(indent->size() - 4);
        os << *indent << "]";
        break;
    }

    case json::kind::string:
    {
        os << json::serialize(jv.get_string());
        break;
    }

    case json::kind::uint64:
    case json::kind::int64:
    case json::kind::double_:
        os << jv;
        break;

    case json::kind::bool_:
        if(jv.get_bool())
            os << "true";
        else
            os << "false";
        break;

    case json::kind::null:
        os << "null";
        break;
    }

    if(indent->empty())
        os << "\n";
}

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
	editor_layer->register_tool("File/Open Level...", [this] { return load_level(); });

	editor_layer->register_tool("Window/Toolbar", [this] { return show_toolbar(); });
	editor_layer->set_tool_checked("Window/Toolbar", true);

	editor_layer->register_tool("Window/Scene", [this] { return show_scene(); });
	editor_layer->set_tool_checked("Window/Scene", true);
	editor_layer->register_tool("Editor/Test JSON Window", [this] {
		bool is_open = true;
		if (ImGui::Begin("Test JSON Window", &is_open)) {
			if (selected_entity != entt::null) {
				if (auto* desc = registry_sp->try_get<rnd::geometry_desc>(selected_entity)) {
					json::object data;
					desc->serialize(data);
					std::ostringstream oss;
					pretty_print(oss, data);
					std::string json_string = oss.str();
					ImGui::InputTextMultiline("JSON", &json_string, ImVec2(500, 500), ImGuiInputTextFlags_ReadOnly);

					rnd::geometry_desc test_desc;
					test_desc.deserialize(desc_system, data);

					ASSERT_MSG(desc->vertices == test_desc.vertices, "arrays are different");
				}
			}
		}
		ImGui::End();

		if (ImGui::Begin("Test files Reload", &is_open)) {
			static std::shared_ptr<res::text_resource> test_res;
			static bool is_show_file_dialog = true;

			if (ImGui::Button("Test Load")) {
				is_show_file_dialog = true;
			}

			if (is_show_file_dialog) {
				if (file_dialog.show("Select text file", &is_show_file_dialog)) {
					auto relateve = file_dialog.get_selected_path().lexically_relative(file_dialog.get_base_path());
					res::tag test_tag = res::tag::make(relateve.string());
					test_res = res::get_system().require<res::text_resource>(test_tag).get_sync();
					res::get_system().watch(test_tag, this, [this] (const res::tag& test_tag) {
						auto updated_res = res::get_system().require<res::text_resource>(test_tag).get_sync();
						egLOG("Editor/Test JSON Window", "Resource '{}' reloaded!", test_tag.string());
						if (updated_res) {
							test_res = updated_res;
						}
					});
				}
			}

			if (test_res) {
				ImGui::Text("TEXT:%s", test_res->c_str());
			}
		}
		ImGui::End();

		return is_open;
	});
	editor_layer->set_tool_checked("Editor/Test JSON Window", false);

	editor_layer->register_tool("Editor/Test ECS window", [this] { return show_ecs_test(); });
	editor_layer->set_tool_checked("Editor/Test ECS window", false);

	editor_layer->register_tool("Editor/Crossing Game", [this] { return show_crossing_game_window(); });
	editor_layer->set_tool_checked("Editor/Crossing Game", true);

	editor_layer->register_implicit("EDITOR/IMPL/SHOW_WEP", [this] { return show_web(); });

	editor_layer->register_tool("Editor/Clear", [this] { return show_clear_cache(); });
	editor_layer->register_tool("Window/Textures", [this] { return show_textures(); });
	editor_layer->set_tool_checked("Window/Textures", false);
	editor_layer->register_tool("Editor/Draw web", [this] { return true; });
	editor_layer->set_tool_checked("Editor/Draw web", is_show_web);


	m_lvl_manager = data.require_shared<scn::level_manager>();
	auto& sfactory = data.require<ecs::system_factory>();
	sfactory.register_automatic_system<edt::spawn_system>("edt::spawn_system");

	auto& rndsys = data.require<rnd::render_system>();
	renderer_sp = std::make_shared<scn::renderer_3d>(data.require<scn::skinning_manager>());
	rndsys.activate_renderer(renderer_sp);

	auto& inp_sys = data.require<inp::input_system>();
	inp_sys.push_input_layer(inp_sys.get_focused_window(), inp::input_layer{ input, true });
	ecs_input = data.require_shared<inp::ecs_input_manager>();
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
		static auto desc_test_res = [this]()
			{
				auto handle = res::get_system().require<editor_test_desc>(res::tag::make("test_desc.desc"));
				return handle;
			}();

		static bool is_first = [&]()-> bool {
			res::get_system().watch(res::tag::make("test_desc.desc"), this, [this](const res::tag&) {
				egLOG("Editor/Test DESC Window", "Resource 'test_desc.desc' reloaded!");
				desc_test_res = res::get_system().require<editor_test_desc>(res::tag::make("test_desc.desc"));
			});
			return true;
			} ();

		if (desc_test_res.is_ready()) {
			auto ttt = desc_test_res.get();
			ImGui::Text("DESC TEST just_number: %d", ttt->just_number);
			ImGui::Text("DESC TEST just_string: %s", ttt->just_string.c_str());
			ImGui::Text("DESC TEST just_float: %f", ttt->just_double);
			if (ttt->field.is_ready()) {
				ImGui::Text("DESC TEST field: %s", ttt->field.get()->field_string.c_str());
			}
		}

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
				auto& world = lvl.get_world("3d_scene");
				registry_sp = std::shared_ptr<entt::registry>(&world.state(), [] (entt::registry*) {});
				renderer_sp->set_current_registry(registry_sp);
			}
		}
	}
	return is_open;
}

bool edt::editor_system::show_crossing_game_window()
{
	bool is_open = true;
	ImGui::SetNextWindowSize(ImVec2(700, 700), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Crossing Game", &is_open, ImGuiWindowFlags_MenuBar ))
	{
		static bool is_menu_bar_click = false;
		static float time = 0.f;
		static bool is_enable_auto_pause = false;
		static bool is_open_reset_dialog = false;
		static bool is_enable_ai_player = false;
		constexpr glm::vec2 grid_size = glm::vec2(100.f, 100.f);
		ImGui::BeginMenuBar();
		if (ImGui::Button("R"))
			is_open_reset_dialog = true;

		if (ImGui::Button(crossingcontext.is_paused ? ">" : "II")) {
			crossingcontext.is_paused = !crossingcontext.is_paused;
		}
		ImGui::SetNextItemWidth(100.f);
		ImGui::SliderFloat("aircraft speed", &crossingcontext.hint.aircraft_speed, 0.1f, glm::length(grid_size));
		ImGui::SetNextItemWidth(100.f);
		ImGui::SliderFloat("ships speed", &crossingcontext.hint.ship_speed, 0.1f, glm::length(grid_size));
		ImGui::Checkbox("Auto Pause", &is_enable_auto_pause);
		ImGui::Checkbox("Enable AI", &is_enable_ai_player);
		ImGui::Text("Time: %.2fs", time + ImGui::GetIO().DeltaTime);
		ImGui::EndMenuBar();

		if (is_open_reset_dialog) {
			ImGui::OpenPopup("Is reset game?");
			is_menu_bar_click = is_open_reset_dialog;
		}
		ImVec2 windowPos = ImGui::GetWindowPos();
		glm::vec2 window_pos = glm::vec2(windowPos.x, windowPos.y);
		ImVec2 windowSize = ImGui::GetWindowSize();
		glm::vec2 window_size = glm::vec2(windowSize.x, windowSize.y);
		ImVec2 contentRegionAvailable = ImGui::GetContentRegionAvail();
		glm::vec2 content_size = glm::vec2(contentRegionAvailable.x, contentRegionAvailable.y);

		ImGui::SetNextWindowSize(ImVec2(300, 50), ImGuiCond_Always);
		ImGui::SetNextWindowPos(ImVec2(window_pos.x + content_size.x / 2 - 150, window_pos.y + content_size.y / 2 - 50));
		if (ImGui::BeginPopupModal("Is reset game?", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDecoration))
		{
			ImGui::Text("Are you sure you want to reset the game?");
			ImVec2 pos = ImGui::GetCursorScreenPos();
			ImVec2 content_size = ImGui::GetContentRegionAvail();
			ImGui::SetCursorScreenPos(ImVec2(pos.x + content_size.x / 2 - 50, pos.y + content_size.y - 15));
			if (ImGui::Button("Yes")) {
				crossingcontext = crossing_context{};
				time = 0.f;
				ImGui::CloseCurrentPopup();
				is_open_reset_dialog = false;
			}
			ImGui::SameLine();
			if (ImGui::Button("No")) {
				ImGui::CloseCurrentPopup();
				is_open_reset_dialog = false;
			}
			ImGui::EndPopup();
		}
		
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			glm::vec2 header = { window_size.x, 16 }; 
			glm::vec2 mouse_pos = glm::vec2(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);
			mouse_pos -= window_pos;
			if (mouse_pos.x > 0 &&
				mouse_pos.y > 0 &&
				mouse_pos.x < header.x && 
				mouse_pos.y < header.y) {
				
				glm::vec2 delta = { ImGui::GetIO().MouseDelta.x, ImGui::GetIO().MouseDelta.y };
				ImGui::SetWindowPos(ImVec2(window_pos.x + delta.x, window_pos.y + delta.y));
				is_menu_bar_click = true;
			}
			glm::vec2 menubar = { window_size.x, 50 };
			if (mouse_pos.x > 0 &&
				mouse_pos.y > 0 &&
				mouse_pos.x < menubar.x &&
				mouse_pos.y < menubar.y) {
				is_menu_bar_click = true;
			}
		}

		ImVec2 pos = ImGui::GetCursorScreenPos();
		glm::vec2 content_pos = glm::vec2(pos.x, pos.y);
		const glm::vec2 grid_to_window = grid_size / content_size;

		const auto GridToWindow = [&](const glm::vec2& grid_pos) -> glm::vec2 {
			return content_pos + grid_pos / grid_to_window;
			};
		const auto DrawShip = [&](const glm::vec2& ship_pos, const glm::vec2& dir, ImColor color) {
			glm::vec2 w_ship_pos = GridToWindow(ship_pos);
			float radius = glm::length(content_size) * 0.01f;

			glm::vec2 front = dir * radius;
			glm::vec2 left = glm::vec2(-front.y, front.x) * 0.3f;
			glm::vec2 right = glm::vec2(front.y, -front.x) * 0.3f;

			ImVec2 p0 = ImVec2(w_ship_pos.x + front.x, w_ship_pos.y + front.y);
			ImVec2 p1 = ImVec2(w_ship_pos.x + left.x, w_ship_pos.y + left.y);
			ImVec2 p2 = ImVec2(w_ship_pos.x + right.x, w_ship_pos.y + right.y);
			ImGui::GetWindowDrawList()->AddTriangleFilled(p0, p1, p2, color);

			front *= 2.f;

			ImVec2 q1 = p1;
			ImVec2 q2 = p2;
			ImVec2 q3 = ImVec2(p2.x - front.x, p2.y - front.y);
			ImVec2 q4 = ImVec2(p1.x - front.x, p1.y - front.y);
			ImGui::GetWindowDrawList()->AddQuadFilled(q1, q2, q3, q4, color);
		};

		const auto DrawAircraft = [&](const glm::vec2& aircraft_pos, const glm::vec2& dir, ImColor color) {
			float radius = glm::length(content_size) * 0.01f;
			glm::vec2 front_world = dir * radius;
			glm::vec2 front = GridToWindow(aircraft_pos) + front_world;
			glm::vec2 left = GridToWindow(aircraft_pos) + glm::vec2(-front_world.y, front_world.x) - front_world * 2;
			glm::vec2 right = GridToWindow(aircraft_pos) + glm::vec2(front_world.y, -front_world.x) - front_world * 2;

			ImVec2 p0 = ImVec2(front.x, front.y);
			ImVec2 p1 = ImVec2(left.x, left.y);
			ImVec2 p2 = ImVec2(right.x, right.y);
			ImGui::GetWindowDrawList()->AddTriangleFilled(p0, p1, p2, color);
			};

		const auto calculate_future_ship_position = [&](const glm::vec2& ship, const glm::vec2& ship_speed, const glm::vec2& aircraft, float aircraft_speed){

			glm::vec2 future_ship = ship;
			glm::vec2 dxy = aircraft - ship;
			glm::vec2 vxy = ship_speed;

			glm::vec2 b = 2 * vxy * dxy;
			glm::vec3 a = glm::pow2(glm::vec3(vxy, aircraft_speed));
			glm::vec2 c = glm::pow2(dxy);

			float D = (pow(b.x + b.y, 2) - 4 * (a.x + a.y - a.z) * (c.x + c.y));
			if (D > 0) {
				float t = (-(b.x + b.y) + sqrt(D)) / (2 * (a.x + a.y - a.z));
				future_ship = ship + vxy * -t;
			}

			return std::pair{ future_ship, D > 0 };
		};

		auto& ships = crossingcontext.ships;
		auto& ships_dirs = crossingcontext.ships_dirs;
		auto& alive_ships = crossingcontext.alive_ships;
		if (!crossingcontext.is_initialized)
		{
			for (int i = 0; i < ships.capacity(); ++i) {
				ships.push_back(glm::linearRand(glm::zero<glm::vec2>(), grid_size));
			}
			for (int i = 0; i < ships_dirs.capacity(); ++i) {
				ships_dirs.push_back(glm::circularRand(1.f));
			}
			for (int i = 0; i < alive_ships.capacity(); ++i) {
				alive_ships.push_back(true);
			}
			crossingcontext.is_initialized = true;
		}

		ImGui::GetWindowDrawList()->AddRectFilled(
			ImVec2(content_pos.x, content_pos.y),
			ImVec2(content_pos.x + content_size.x, content_pos.y + content_size.y),
			ImColor(0, 0, 255, 200)
		);

		for (int i = 0; i <= grid_size.x; i += 5) {
			glm::vec2 p1 = GridToWindow(glm::vec2(i, 0.f));
			glm::vec2 p2 = GridToWindow(glm::vec2(i, grid_size.y));
			ImGui::GetWindowDrawList()->AddLine(
				ImVec2(p1.x, p1.y),
				ImVec2(p2.x, p2.y),
				ImColor(255, 255, 255, 200), 0.5f
			);
			for (int j = i + 1; j < i + 5; ++j) {
				glm::vec2 p1 = GridToWindow(glm::vec2(j, 0.f));
				glm::vec2 p2 = GridToWindow(glm::vec2(j, grid_size.y));
				ImGui::GetWindowDrawList()->AddLine(
					ImVec2(p1.x, p1.y),
					ImVec2(p2.x, p2.y),
					ImColor(255, 255, 255, 150), 0.2f
				);
			}
		}

		for (int i = 0; i <= grid_size.y; i += 5) {
			glm::vec2 p1 = GridToWindow(glm::vec2(0.f, i));
			glm::vec2 p2 = GridToWindow(glm::vec2(grid_size.x, i));
			ImGui::GetWindowDrawList()->AddLine(
				ImVec2(p1.x, p1.y),
				ImVec2(p2.x, p2.y),
				ImColor(255, 255, 255, 200), 0.5f
			);
			for (int j = i + 1; j < i + 5; ++j) {
				glm::vec2 p1 = GridToWindow(glm::vec2(0.f, j));
				glm::vec2 p2 = GridToWindow(glm::vec2(grid_size.x, j));
				ImGui::GetWindowDrawList()->AddLine(
					ImVec2(p1.x, p1.y),
					ImVec2(p2.x, p2.y),
					ImColor(255, 255, 255, 150), 0.2f
				);
			}
		}

		for (const auto& ship : ships) {
			if (!alive_ships[&ship - &ships[0]]) continue;
			DrawShip(ship, ships_dirs[&ship - &ships[0]], ImColor(255, 0, 0, 255));
		}

		static bool is_dragging = false;
		glm::vec2 mouse_pos = glm::vec2(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y) - content_pos;

		if ((!is_enable_ai_player || !crossingcontext.is_aircraft_placed) && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !is_menu_bar_click)
		{
			if (!is_dragging && !crossingcontext.is_aircraft_placed)
				crossingcontext.aircraft = mouse_pos * grid_to_window;
			crossingcontext.aircraft_dir = glm::normalize((mouse_pos * grid_to_window + 0.00001f) - crossingcontext.aircraft);
			crossingcontext.is_aircraft_placed = true;
			is_dragging = !is_enable_ai_player;
		}

		static glm::vec2 move_to = glm::zero<glm::vec2>();

		if (is_dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			is_dragging = false;
			move_to = mouse_pos * grid_to_window;
		}

		if (is_enable_auto_pause && glm::length(crossingcontext.aircraft - move_to) < 0.1f)
		{
			crossingcontext.is_paused = true;
		}

		if (crossingcontext.is_aircraft_placed)
		{
			if (is_dragging)
			{
				DrawAircraft(mouse_pos* grid_to_window, crossingcontext.aircraft_dir, ImColor(255, 255, 0, 255));
				glm::vec2 aircraft_window = GridToWindow(crossingcontext.aircraft);
			}			
			DrawAircraft(crossingcontext.aircraft, crossingcontext.aircraft_dir, ImColor(0, 255, 0, 255));
		}

		if (!crossingcontext.is_paused)
		{
			const float speed = crossingcontext.hint.aircraft_speed;
			crossingcontext.aircraft += crossingcontext.aircraft_dir * speed * ImGui::GetIO().DeltaTime;
			for (auto& ship : ships) {
				if (!alive_ships[&ship - &ships[0]]) continue;
				ship += ships_dirs[&ship - &ships[0]] * crossingcontext.hint.ship_speed * ImGui::GetIO().DeltaTime;
				// Wrap around the grid
				if (ship.x < 0.f) ship.x += grid_size.x;
				if (ship.x > grid_size.x) ship.x -= grid_size.x;
				if (ship.y < 0.f) ship.y += grid_size.y;
				if (ship.y > grid_size.y) ship.y -= grid_size.y;
			}

			// Check for collisions with ships
			for (const auto& ship : ships) {
				int idx = &ship - &ships[0];
				if (!alive_ships[idx]) continue;
				float length = glm::length(crossingcontext.aircraft - ship);
				if (length < 1.f) {
					alive_ships[&ship - &ships[0]] = false;
					break;
				}
			}
			// Wrap around the grid
			if (crossingcontext.aircraft.x < 0.f) crossingcontext.aircraft.x += grid_size.x;
			if (crossingcontext.aircraft.x > grid_size.x) crossingcontext.aircraft.x -= grid_size.x;
			if (crossingcontext.aircraft.y < 0.f) crossingcontext.aircraft.y += grid_size.y;
			if (crossingcontext.aircraft.y > grid_size.y) crossingcontext.aircraft.y -= grid_size.y;
		}
		if (crossingcontext.is_aircraft_placed) {
			crossingcontext.hint.nearest_ship_distance = std::numeric_limits<float>::max();
			crossingcontext.hint.nearest_ship_index = -1;
			for (const auto& ship : ships) {
				int idx = &ship - &ships[0];
				if (!alive_ships[idx]) continue;
				float length = glm::length(crossingcontext.aircraft - ship);
				if (length < crossingcontext.hint.nearest_ship_distance) {
					crossingcontext.hint.nearest_ship_distance = length;
					crossingcontext.hint.nearest_ship_index = idx;
				}
			}
			
			if (crossingcontext.hint.nearest_ship_index != -1) {
				ImGui::Text("Nearest ship index: %d", crossingcontext.hint.nearest_ship_index);
				ImGui::Text("Distance to nearest ship: %.2f", crossingcontext.hint.nearest_ship_distance);
			} else {
				ImGui::Text("No nearby ships.");
			}

			if (crossingcontext.hint.nearest_ship_index != -1) {
				const auto& hint = crossingcontext.hint;
				const auto& aircraft_pos = crossingcontext.aircraft;
				const auto& nearest_ship = ships[hint.nearest_ship_index];
				const auto& nearest_ship_dir = ships_dirs[hint.nearest_ship_index];

				auto [future_ship, exist] = calculate_future_ship_position( 
					nearest_ship, hint.ship_speed * nearest_ship_dir, aircraft_pos, hint.aircraft_speed);

				if (exist && is_enable_ai_player)
				{
					crossingcontext.aircraft_dir = glm::normalize(future_ship - aircraft_pos);
					move_to = future_ship;
				}

				if (future_ship.x < 0.f) future_ship.x += grid_size.x;
				if (future_ship.x > grid_size.x) future_ship.x -= grid_size.x;
				if (future_ship.y < 0.f) future_ship.y += grid_size.y;
				if (future_ship.y > grid_size.y) future_ship.y -= grid_size.y;

				ImVec2 aircraft = ImVec2(
					GridToWindow(crossingcontext.aircraft).x,
					GridToWindow(crossingcontext.aircraft).y
				);
				
				if (exist) {
					DrawShip(future_ship, nearest_ship_dir, ImColor(255, 255, 255, 154));
				}

				if (crossingcontext.is_paused) {
					glm::vec2 speed2d = glm::vec2{ hint.aircraft_speed, hint.aircraft_speed } / grid_to_window;
					glm::vec2 shipspeed2d = glm::vec2{ hint.ship_speed, hint.ship_speed } / grid_to_window;

					ImGui::GetWindowDrawList()->AddCircle(aircraft, glm::length(crossingcontext.aircraft_dir * speed2d), ImColor(255, 255, 0, 255));
				}
			}
		} else {
			ImGui::Text("Aircraft doesn't placed.");
		}

		if (!crossingcontext.is_paused) {
			time += ImGui::GetIO().DeltaTime;
		}
		else {
			glm::vec2 pause_pos = grid_size;
			pause_pos.x -= grid_size.x / 2;
			pause_pos.y -= 5;
			pause_pos = GridToWindow(pause_pos);
			ImGui::SetCursorScreenPos(ImVec2(pause_pos.x, pause_pos.y));
			ImGui::SetWindowFontScale(glm::length((glm::vec2(0.2, 0.2) / grid_to_window)));
			ImGui::TextColored(ImColor(255, 0, 0), "PAUSE");
			ImGui::SetWindowFontScale(1);
		}

		

		is_menu_bar_click = false;

	}
	ImGui::End();
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

bool edt::editor_system::show_ecs_test()
{
	init_ecs_test();

	bool is_open = true;
	ImGui::SetNextWindowSize(ImVec2{ 400, 300 }, ImGuiCond_FirstUseEver);
	if (ImGui::Begin("ECS Test", &is_open))
	{
		for (const auto ent : registry_sp->view<eng::transform3d, glm::vec2>()) {
			auto& trn = registry_sp->get<eng::transform3d>(ent);
			auto& v = registry_sp->get<glm::vec2>(ent);
			ImGui::Text("entity: %d, pitch: %.3f, yaw: %.3f, roll: %.3f", ent, trn.get_pitch(), trn.get_yaw(), trn.get_roll());
			auto pos = trn.get_pos();
			ImGui::Text("\t\tx: %.3f, y: %.3f, z: %.3f", pos.x, pos.y, pos.z);
			ImGui::Text("\t\tx: %.3f, y: %.3f", v.x, v.y);
		}

		ImGui::Separator();
		ImGui::NewLine();

		for (auto ent : registry_sp->view<eng::transform3d>()) {
			auto& trn = registry_sp->get<eng::transform3d>(ent);
			ImGui::Text("entity: %d, pitch: %.3f, yaw: %.3f, roll: %.3f", ent, trn.get_pitch(), trn.get_yaw(), trn.get_roll());
			auto pos = trn.get_pos();
			ImGui::Text("\t\tx: %.3f, y: %.3f, z: %.3f", pos.x, pos.y, pos.z);
		}

		ImGui::End();
	}

	return is_open;
}

void recurcive_set(entt::registry* registry_sp, const std::vector<uint32_t>& res, entt::entity ent, const ds::color& color, bool reset = true) {
	if (registry_sp->all_of<scn::material_desc_component, scn::mesh_component>(ent)) {
		auto mesh = registry_sp->get<scn::mesh_component>(ent);
		auto& hightlight = registry_sp->get_or_emplace<scn::hightlight_component>(ent);
		hightlight.color = color;
		if (hightlight.triangles.size() != mesh.mesh.get_indices_count() / 3)
			hightlight.triangles.resize(mesh.mesh.get_indices_count() / 3, std::numeric_limits<uint32_t>::max());
		if (reset) {
			hightlight.triangles.clear();
			hightlight.triangles.resize(mesh.mesh.get_indices_count() / 3, std::numeric_limits<uint32_t>::max());
			size_t offset_begin = mesh.mesh.ind_begin / 3;
			size_t offset_end = mesh.mesh.ind_end / 3;
			for (const auto& triangle : res) {
				if ((triangle >= offset_begin && triangle < offset_end)) {
					uint32_t local_idx = triangle - offset_begin;
					hightlight.triangles[local_idx] = local_idx;
				}
			}
		}
	}

	if (registry_sp->all_of<scn::children_component>(ent)) {
		auto children = registry_sp->get<scn::children_component>(ent);
		for (const auto& child : children.children) {
			recurcive_set(registry_sp, res, child, color, reset);
		}
	}
}

bool edt::editor_system::show_textures()
{
	bool is_open = true;
	if (ImGui::Begin("Textures", &is_open))
	{
		std::vector<std::string> list;
		std::vector<res::tag> list_tags;
		std::size_t max_name = 0;
		for (const auto& [key, _] : rnd::get_system().get_texture_manager().get_cache())
		{
			list_tags.push_back(key);
			list.push_back(std::string(key.pure_name()));
			max_name = std::max(max_name, list.back().size());
		} 

		auto items_getter = [](void* data, int idx) -> const char*
			{
				std::vector<std::string>* list = (std::vector<std::string>*)data;

				if (idx < 0 && idx >= list->size()) {
					nullptr;
				}
				return (*list)[idx].c_str();
			};
		ImVec2 windowSize = ImGui::GetContentRegionAvail();
		ImVec2 pos1 = ImGui::GetCursorScreenPos();
		float itemHeight = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;

		int visibleItems = static_cast<int>((windowSize.y - ImGui::GetStyle().ItemSpacing.y) / itemHeight);

		static int item_current = 1;
		ImGui::PushItemWidth(max_name * 10);
		static ds::color picker_color{ 0.0f, 1.0f, 0.0f, 1.0f };
		bool is_hightlight_color_changed = ImGui::ColorEdit4("Highlight Color", glm::value_ptr(picker_color));		
		ImGui::ListBox("##textures_listbox", &item_current, items_getter, (void*)&list, list.size(), std::min(visibleItems, (int)list.size()));
		ImGui::PopItemWidth();

		if (item_current >= 0 && item_current < list_tags.size())
		{
			glm::vec2 pos{ pos1.x + max_name * 10, pos1.y };
			glm::vec2 contentRegionAvailable{ windowSize.x - max_name * 10, windowSize.y };
			auto texture = rnd::get_system().get_texture_manager().find(list_tags[item_current]);
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
			ImGui::ImageButton("##TextureImg", backend->get_imgui_texture_from_texture(texture), ImVec2{ contentRegionAvailable.x, contentRegionAvailable.y }, ImVec2(0, 1), ImVec2(1, 0));

			style.Colors[ImGuiCol_Button] = original_button_color;
			style.Colors[ImGuiCol_ButtonHovered] = original_button_hovered_color;
			style.Colors[ImGuiCol_ButtonActive] = original_button_active_color;
			style.FramePadding = original_padding;

			static ds::bbox rect;
			static bool is_dragging = false;
			if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsItemHovered() && !is_dragging) {
				ds::point2d mouse_pos{ ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y };
				mouse_pos -= pos;
				mouse_pos /= ds::point2d{ contentRegionAvailable.x, contentRegionAvailable.y };

				mouse_pos.y = 1.0f - mouse_pos.y; // flip y
				rect.min = rect.max = mouse_pos;
				is_dragging = true;
			}

			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsItemHovered() && !is_dragging) {
				ds::point2d mouse_pos{ ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y };
				mouse_pos -= pos;
				mouse_pos /= ds::point2d{ contentRegionAvailable.x, contentRegionAvailable.y };

				mouse_pos.y = 1.0f - mouse_pos.y; // flip y

				std::vector<uint32_t> res;
				{
					ds::scoped_timer timer("rtree point query time");
					ds::bbox rect{ mouse_pos, mouse_pos };
					res = rtree.query(rect);
				}

				for (const auto& triangle : res) {
					std::cout << "Triangle: " << triangle << std::endl;
				}

				registry_sp->clear<scn::hightlight_component>();
				recurcive_set(registry_sp.get(), res, backpackent, picker_color);
				is_hightlight_color_changed = false;
			}

			if (is_dragging) {
				ds::point2d mouse_pos{ ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y };
				mouse_pos -= pos;
				mouse_pos /= ds::point2d{ contentRegionAvailable.x, contentRegionAvailable.y };

				mouse_pos.y = 1.0f - mouse_pos.y; // flip y
				rect.max = mouse_pos;

				rect.max.x = std::clamp(rect.max.x, 0.0f, 1.0f);
				rect.max.y = std::clamp(rect.max.y, 0.0f, 1.0f);
				rect.min.x = std::clamp(rect.min.x, 0.0f, 1.0f);
				rect.min.y = std::clamp(rect.min.y, 0.0f, 1.0f);

				ImDrawList* draw_list = ImGui::GetWindowDrawList();

				ds::point2d p_min_norm = rect.min;
				float screen_min_y = 1.0f - p_min_norm.y;
				float screen_min_x = p_min_norm.x;

				ds::point2d p_max_norm = rect.max;
				float screen_max_y = 1.0f - p_max_norm.y;
				float screen_max_x = p_max_norm.x;

				ImVec2 p1(
					pos.x + screen_min_x * contentRegionAvailable.x,
					pos.y + screen_min_y * contentRegionAvailable.y
				);

				ImVec2 p2(
					pos.x + screen_max_x * contentRegionAvailable.x,
					pos.y + screen_max_y * contentRegionAvailable.y
				);

				draw_list->AddRect(p1, p2, IM_COL32(255, 255, 0, 255), 0.0f, 0, 2.0f);

				draw_list->AddRectFilled(p1, p2, IM_COL32(255, 255, 0, 50));
			}

			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && is_dragging) {

				if (rect.min.x > rect.max.x) {
					std::swap(rect.min.x, rect.max.x);
				}
				if (rect.min.y > rect.max.y) {
					std::swap(rect.min.y, rect.max.y);
				}

				std::vector<uint32_t> res;
				{
					//ds::svg_writer query_svg{ "rtree_query_visualization2.svg", 2048, 2048 };
					ds::scoped_timer timer("rtree rect query time");
					res = rtree.query(rect/*, [&](const auto& node, uint32_t depth) {
						query_svg.add_rect(node.box, "red", depth);
						if (node.is_leaf) {
							for (const auto& e : node.entries) {
								if (ds::intersects(e.box, rect))
									query_svg.add_rect(e.box, "green", depth);
							}
						}

					}*/);
				}

				registry_sp->clear<scn::hightlight_component>();
				recurcive_set(registry_sp.get(), res, backpackent, picker_color);
				is_dragging = false;
				is_hightlight_color_changed = false;
				rect = ds::bbox{};
			}

			if (is_hightlight_color_changed) {
				recurcive_set(registry_sp.get(), {}, backpackent, picker_color, false);
			}

		}
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

void edt::editor_system::draw_manipulator(const glm::vec2& pos, const glm::vec2& size)
{

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

bool edt::editor_system::init_ecs_test()
{
	if (is_inited_ecs_test) {
		return true;
	}

	is_inited_ecs_test = true;
	ecs::entity first = registry_sp->create();
	eng::transform3d trn{ glm::translate(glm::mat4{1.0}, glm::vec3(6)) };
	registry_sp->emplace<eng::transform3d>(first, trn);
	glm::vec2 tmp{ 9, 9 };
	registry_sp->emplace<glm::vec2>(first, tmp);
	trn.set_pos(glm::vec3{ 6, 6, 6 });
	ecs::entity second = registry_sp->create();
	eng::transform3d trn2{ glm::translate(glm::mat4{1.0}, glm::vec3(3)) };
	registry_sp->emplace<eng::transform3d>(second, trn2);
	trn2.add_yaw(45.f);

	return true;
}
