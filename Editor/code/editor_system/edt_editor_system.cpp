#include "edt_editor_system.h"
#include "gui_api.hpp"
#include "application.h"
#include "gs_game_system.h"
#include "res_system.h"
#include "res_picture.h"
#include "rnd_render_system.h"
#include "scn_primitives.h"
#include "scn_camera_component.hpp"
#include "scn_camera_controller_component.hpp"
#include "ecs_common_system.h"
#include "scn_primitives.h"
#include "eng_transform_3d.hpp"
#include "wnd_window_system.h"
#include "scn_material_component.hpp"
#include "edt_input_manager.h"
#include "edt_guizmo.hpp"
#include <misc/cpp/imgui_stdlib.h>
#include <boost/json.hpp>
#include <imgui.h>

void
pretty_print( std::ostream& os, json::value const& jv, std::string* indent = nullptr )
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

editor::EditorSystem::EditorSystem(desc::desc_system& desc_system_)
	: desc_system(desc_system_)
{
	GUI_REG_LAMBDA("File/Import...", [this] { return show_file_dialog(); });

	GUI_REG_LAMBDA("Window/Toolbar", [this] { return show_toolbar(); });
	GUI_SET_ITEM_CHECKED("Window/Toolbar", true);

	GUI_REG_LAMBDA("Window/Scene", [this] { return show_scene(); });
	GUI_SET_ITEM_CHECKED("Window/Scene", true);
	GUI_REG_LAMBDA("Editor/Test JSON Window", [this] { 
		bool is_open = true;
		if (ImGui::Begin("Test JSON Window", &is_open)) {
			ImGui::Text("This is a test window for JSON serialization.");
			if (test_json_selected_material != entt::null) {
				auto obj = scn::convert_material_to_json(test_json_selected_material); 
				std::ostringstream oss;
				pretty_print(oss, obj);
				std::string json_string = oss.str();
				ImGui::InputTextMultiline("JSON", &json_string, ImVec2(500, 500), ImGuiInputTextFlags_ReadOnly); 
			}

			ImGui::End();
		}
		return is_open;
	});
	GUI_SET_ITEM_CHECKED("Editor/Test JSON Window", true);

	GUI_REG_LAMBDA("Editor/Test ECS window", [this] { return show_ecs_test(); });
	GUI_SET_ITEM_CHECKED("Editor/Test ECS window", false);

	GUI_REG_LAMBDA_IMPLICIT("EDITOR/IMPL/SHOW_WEP", [this] { return show_web(); });

	GUI_REG_LAMBDA("Editor/Clear", [this] { return show_clear_cache(); });
	GUI_REG_LAMBDA("Window/Materials", [this] { return show_materials(); });
	GUI_SET_ITEM_CHECKED("Window/Materials", true);
	GUI_REG_LAMBDA("Window/Textures", [this] { return show_textures(); });
	GUI_SET_ITEM_CHECKED("Window/Textures", true);
	GUI_REG_LAMBDA("Editor/Draw web", [this] { return true; });
	GUI_SET_ITEM_CHECKED("Editor/Draw web", is_show_web);


	auto logo = res::get_system().require_resource<res::Picture>(res::tag::make("icons/editor_engine_logo.png"));
	gs::get_system().get_window()->set_logo(logo);
	gs::get_system().get_window()->set_title("Snake Editor");
	gui::get_system().set_show_title_bar(true);
	gui::get_system().set_show_title_bar_dbg(true);

	input = std::make_shared<edt::input_manager>();
	input->unblock_layer_once();
	inp::get_system().activate_manager(input);

	file_dialog.set_current_path(res::get_system().get_resources_path());

	gs::get_system().get_window()->eventResizeWindow.subscribe([](wnd::window&, int w, int h)
	{
		//for (auto& ent : ecs::filter<scn::camera_component>())
		//{
		//	auto* camera = ecs::get_component<scn::camera_component>(ent);
		//	camera->viewport.size = glm::ivec2(w, h);
		//}
	});

	auto txt = rnd::get_system().get_texture_manager().generate_texture(res::tag(res::tag::memory, "__black"), {1,1}, rnd::driver::texture_header::TYPE::RGB8, {0, 0, 0});
	auto txt2 = rnd::get_system().get_texture_manager().generate_texture(res::tag(res::tag::memory, "__red"), {1,1}, rnd::driver::texture_header::TYPE::RGB8, {255, 0, 0});

	auto anchors = ecs::registry.view<scn::scene_anchor_component>();
	ecs::entity world_anchor;
	if (anchors.empty()) {
		world_anchor = ecs::create_entity();
		ecs::registry.emplace<scn::scene_anchor_component>(world_anchor);
		ecs::registry.emplace<scn::name_component>(world_anchor, scn::name_component{ .name = "Anchor" });
	}
	else {
		world_anchor = anchors.front();
	}
	//TODO: make separate render pipline for editor things
	decltype(scn::children_component::children) children;
	if (ecs::registry.all_of<scn::children_component>(world_anchor)) {
		auto& kids = ecs::registry.get<scn::children_component>(world_anchor);
		children = kids.children;
		ecs::remove_component<scn::children_component>(world_anchor);
	}

	// black base material
	ecs::entity black_mlt = ecs::create_entity();
	ecs::registry.emplace<scn::base_material_component>(black_mlt, scn::base_material_component{ .albedo = ds::color(glm::vec3(0), 1) });
	ecs::registry.emplace<scn::name_component>(black_mlt, scn::name_component{ .name = "Editor web "});
	// chess base material
	ecs::entity light_mlt = ecs::create_entity();
	ecs::registry.emplace<scn::base_material_component>(light_mlt, scn::base_material_component{});
	ecs::registry.emplace<scn::name_component>(light_mlt, scn::name_component{ .name = "LIGHT"});

	ecs::entity window_mlt = ecs::create_entity();
	ecs::registry.emplace<scn::base_material_component>(window_mlt, scn::base_material_component{});
	ecs::registry.emplace<scn::albedo_map_component>(window_mlt, scn::albedo_map_component{ .txm = res::tag::make("window.png") });
	ecs::registry.emplace<scn::name_component>(window_mlt, scn::name_component{ .name = "WINDOW"});
	ecs::registry.emplace<scn::is_transparent_flag_component>(window_mlt);

	//  camera
	{
		glm::ivec4 viewport{ glm::zero<glm::ivec2>(), gs::get_system().get_window()->get_size() };
		glm::vec3 rotation(0);
		rotation.x = -glm::radians(45.0f);
		auto ecs_entity = ecs::create_entity();
		children.push_back(ecs_entity);
		ecs::registry.emplace<scn::name_component>(ecs_entity, "Editor camera");
		ecs::registry.emplace<scn::parent_component>(ecs_entity, world_anchor);
		ecs::registry.emplace<scn::camera_component>(ecs_entity, scn::camera_component{ .viewport = viewport });
		ecs::registry.emplace<scn::local_transform>(ecs_entity);
		ecs::registry.emplace<scn::world_transform>(ecs_entity);
		ecs::registry.emplace<scn::renderable>(ecs_entity);
		ecs::registry.emplace<scn::mouse_controller_component>(ecs_entity, scn::mouse_controller_component{ .rotation = rotation });
	}

	auto web = scn::generate_web({ 50, 50 });
	res::tag web_tag = res::tag(res::tag::memory, "web.asset");
	res::model_presintation web_model_pres;
	web_model_pres.data = web.data;
	std::shared_ptr<res::Model> web_asset = std::make_shared<res::Model>(web_tag, web_model_pres);
	res::get_system().add_resource(web_asset);
	// web
	{
		editor_web = ecs::create_entity();
		children.push_back(editor_web);

		res::meshes_conteiner& data = web.data;
		res::mesh_view& mesh = web.mesh;
		//mesh.material_id = 0;

		ecs::registry.emplace<scn::model_root_component>(editor_web, scn::model_root_component{ .data = data, .geom_tag = web_tag });
		ecs::registry.emplace<scn::mesh_component>(editor_web, scn::mesh_component{ .mesh = mesh });
		ecs::registry.emplace<scn::name_component>(editor_web, "Editor Web");
		ecs::registry.emplace<scn::parent_component>(editor_web, world_anchor);
		auto& transform = ecs::registry.emplace<scn::local_transform>(editor_web);
		transform.local = glm::scale(glm::vec3(1, 0, 1));
		ecs::registry.emplace<scn::world_transform>(editor_web);
		ecs::registry.emplace<rnd::render_mode_component>(editor_web, rnd::RENDER_MODE::LINE);// TODO: move to material
		ecs::registry.emplace<scn::renderable>(editor_web);
		ecs::registry.emplace<scn::material_link_component>(editor_web, black_mlt );
	}

	auto geom = scn::generate_cube();
	res::tag cube_tag = res::tag(res::tag::memory, "cube.asset");
	res::model_presintation cube_model_pres;
	cube_model_pres.data = geom.data;
	std::shared_ptr<res::Model> cube_asset = std::make_shared<res::Model>(cube_tag, cube_model_pres);
	res::get_system().add_resource(cube_asset);

	// windows objects
	for(int i = 0; i < 1; ++i)
	{
		auto wind = ecs::create_entity();
		children.push_back(wind);

		res::meshes_conteiner& data = geom.data;
		res::mesh_view& mesh = geom.mesh;

		glm::vec2 rnd_pos = glm::diskRand(1.f);

		ecs::registry.emplace<scn::name_component>(wind, scn::name_component{ .name = "Window" });
		ecs::registry.emplace<scn::parent_component>(wind, world_anchor);
		ecs::registry.emplace<scn::model_root_component>(wind, scn::model_root_component{ .data = data, .geom_tag = cube_tag });
		ecs::registry.emplace<scn::mesh_component>(wind, scn::mesh_component{ .mesh = mesh });
		auto& transform = ecs::registry.emplace<scn::local_transform>(wind);
		transform.local = glm::translate(glm::mat4{ 1.0 }, glm::vec3(rnd_pos.x, 0, rnd_pos.y));
		ecs::registry.emplace<scn::world_transform>(wind);
		ecs::registry.emplace<scn::renderable>(wind);
		ecs::registry.emplace<scn::material_link_component>(wind, window_mlt );
	}

	// light
	{
		light = ecs::create_entity();
		children.push_back(light);
		ecs::registry.emplace<scn::directional_light>(light, 
			glm::vec4(-0.2f, -1.0f, -0.3f, 0.0),
			glm::vec4(0.5f, 0.5f, 0.5f, 1.0),
			glm::vec4(0.2f, 0.2f, 0.2f, 1.0),
			glm::vec4(1)
		);

		ecs::registry.emplace<scn::name_component>(light, scn::name_component{ .name = "Global Light" });
		ecs::registry.emplace<scn::parent_component>(light, world_anchor);
		ecs::registry.emplace<scn::renderable>(light);
	}

	// sky
	{
		sky = ecs::create_entity();
		children.push_back(sky);
		auto m = scn::generate_cube();
		ecs::registry.emplace<scn::sky_component>(sky, scn::sky_component{ .data = m.data, .mesh = m.mesh, .cube_map = std::vector<res::tag>{
			res::tag::make("skybox/right.jpg"),
			res::tag::make("skybox/left.jpg"),
			res::tag::make("skybox/bottom.jpg"),
			res::tag::make("skybox/top.jpg"),
			res::tag::make("skybox/front.jpg"),
			res::tag::make("skybox/back.jpg"),
			}
			});
		ecs::registry.emplace<scn::renderable>(sky);
		ecs::registry.emplace<scn::name_component>(sky, scn::name_component{ .name = "Sky" });
		ecs::registry.emplace<scn::parent_component>(sky, world_anchor);
	}

	ecs::registry.emplace<scn::children_component>(world_anchor, children);
}


editor::EditorSystem::~EditorSystem()
{
	inp::get_system().deactivate_manager(input);
}

void editor::EditorSystem::show_tree_items(ecs::entity ent)
{
	std::string obj_idx = std::to_string((int)ent);
	std::string name = "Node##" + obj_idx;
	if (ecs::registry.all_of<scn::name_component>(ent)) {
		auto& com_name = ecs::registry.get<scn::name_component>(ent);
		if (!com_name.name.empty()) {
			name = com_name.name + "##" + obj_idx;
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

			if (ecs::registry.all_of<scn::name_component>(ent)) {
				auto& name_comp = ecs::registry.get<scn::name_component>(ent);
				strncpy(buf, name_comp.name.c_str(), sizeof(buf) - 1);
				buf[sizeof(buf) - 1] = '\0';
				ImGui::OpenPopup("Rename Node");
			}
		}

		if (ImGui::BeginPopup("Rename Node")) {
			if (ImGui::InputText("##rename", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
				if (ecs::registry.all_of<scn::name_component>(ent)) {
					auto& name_comp = ecs::registry.get<scn::name_component>(ent);
					name_comp.name = buf;
				}
				
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		if (parent_node_to_add != entt::null) {
			if (ImGui::BeginPopup("create_new_entity")) {
				if (ImGui::InputText("##new_entity_name", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
					ecs::entity child = ecs::create_entity();
					ecs::registry.emplace<scn::name_component>(child, buf);
					ecs::registry.emplace<scn::parent_component>(child, parent_node_to_add);
					if (ecs::registry.all_of<scn::children_component>(parent_node_to_add)) {
						auto& children = ecs::registry.get<scn::children_component>(parent_node_to_add);
						auto children_list = children.children;
						children_list.push_back(child);
						ecs::registry.remove<scn::children_component>(parent_node_to_add);
						ecs::registry.emplace<scn::children_component>(parent_node_to_add, children_list);
					}
					else {
						ecs::registry.emplace<scn::children_component>(parent_node_to_add, std::vector<ecs::entity>{child});
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
					ecs::registry.emplace<scn::camera_component>(ent, 
						scn::camera_component{ .viewport = glm::ivec4{100,100, 500, 500} }
					);
				}

				if (ImGui::MenuItem("Add Directional Light")) {
					ecs::registry.emplace<scn::directional_light>(ent, 
						glm::vec4(-0.2f, -1.0f, -0.3f, 0.0),
						glm::vec4(0.5f, 0.5f, 0.5f, 1.0),
						glm::vec4(0.2f, 0.2f, 0.2f, 1.0),
						glm::vec4(1.0f)
					);
				}
				if (ecs::registry.all_of<scn::renderable>(ent)) {
					if (ImGui::MenuItem("Remove Render Flag")) {
						ecs::remove_component<scn::renderable>(ent);
					}
				} else {
					if (ImGui::MenuItem("Add Render Flag")) {
						ecs::registry.emplace<scn::renderable>(ent);
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

		if (ecs::registry.all_of<scn::animations_component>(ent)) {
			auto& anims = ecs::registry.get<scn::animations_component>(ent);
			std::string_view play_anim_name;
			if (ecs::registry.all_of<scn::playable_animation>(ent)) {
				auto& play = ecs::registry.get<scn::playable_animation>(ent);
				play_anim_name = play.name;
				ImGui::Checkbox("Is Repeat", &(play.is_repeat_animation));
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
						cur = n;

						if (ecs::registry.all_of<scn::playable_animation>(ent)) {
							auto& play = ecs::registry.get<scn::playable_animation>(ent);
							if (cur == 0) {
								ecs::remove_component<scn::playable_animation>(ent);
							} else {
								play.name = names[cur];
								play.current_tick = 0.f;
							}
						} else if (cur != 0){
							scn::playable_animation play_new;
							play_new.name = names[cur];
							ecs::registry.emplace<scn::playable_animation>(ent, play_new);
						}
					}

					// Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}

		if (ecs::registry.all_of<scn::local_transform>(ent)) {
			auto& trans = ecs::registry.get<scn::local_transform>(ent);
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
				ecs::registry.patch<scn::local_transform>(ent);
			}
		}

		if (ecs::registry.all_of<scn::directional_light>(ent)) {
			auto& color = ecs::registry.get<scn::directional_light>(ent);
			ImGui::ColorEdit3("Light Color", glm::value_ptr(color.diffuse));
			ImGui::ColorEdit3("Ambient Color", glm::value_ptr(color.ambient));
			ImGui::ColorEdit3("Specular Color", glm::value_ptr(color.specular));
			ImGui::DragFloat3("Position", glm::value_ptr(color.direction), 0.01f, -1.0f, 1.0f, "%.1f");
			if (ecs::registry.all_of<scn::material_link_component>(ent)) {
				auto& mlt = ecs::registry.get<scn::material_link_component>(ent);
				if (ecs::registry.all_of<scn::base_material_component>(mlt.material)) {
					auto& base = ecs::registry.get<scn::base_material_component>(mlt.material);
					base.albedo = color.diffuse;
				}
			}
		}

		if (ecs::registry.all_of<scn::camera_component>(ent)) {
			auto& camera = ecs::registry.get<scn::camera_component>(ent);
			ImGui::Text("fov: %3.f", camera.fov);
			ImGui::Text("near: %3.f", camera.near_distance);
			ImGui::Text("far: %3.f", camera.far_distance);
			ImGui::Text("x: %d, y: %d, width: %d, height: %d", camera.viewport.center.x, camera.viewport.center.y, camera.viewport.size.x, camera.viewport.size.y);
		}

		if (ecs::registry.all_of<scn::children_component>(ent)) {
			auto& children = ecs::registry.get<scn::children_component>(ent);
			for (auto& child : children.children)
			{
				show_tree_items(child);
			}
		}

		ImGui::TreePop();
		ImGui::Spacing();
	}
}

bool editor::EditorSystem::show_toolbar()
{
	ImGuiIO& io = ImGui::GetIO();

	bool is_open = true;
	if (ImGui::Begin("Observer", &is_open))
	{
		auto ttt = desc_system.get_desc<editor_test_desc>();
		if (ttt) {
			ImGui::Text("DESC TEST just_number: %d", ttt->just_number);
			ImGui::Text("DESC TEST just_string: %s", ttt->just_string.c_str());
			ImGui::Text("DESC TEST just_float: %f", ttt->just_double);
			if (ttt->field) {
				ImGui::Text("DESC TEST field: %s", ttt->field->field_string.c_str());
			}
		}

		auto& app = app::get_app_system();
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
			for (const auto ent : ecs::registry.view<scn::scene_anchor_component>())
			{
				show_tree_items(ent);
			}
		}

		ImGui::Separator();
		ImGui::NewLine();
		{
			ImGui::Text("Camera");
			ImGui::Separator();
			std::vector<std::string> names;
			std::vector<ecs::entity> cameras;
			int cam_id = 1;
			int cam_cur = 0;
			int cam_cur_id = 0;
			auto cameras_view = ecs::registry.view<scn::camera_component>();
			for (auto& ent : cameras_view)
			{
				cameras.push_back(ent);
				if (ecs::registry.all_of<scn::name_component>(ent)) {
					auto& name = ecs::registry.get<scn::name_component>(ent);
					names.push_back(name.name);
				} else {
					names.push_back("Camera" + std::to_string(cam_id++));
				}

				if (ecs::registry.all_of<scn::renderable>(ent)) {
					cam_cur_id = cam_cur;
				}

				++cam_cur;
			}

			if (ImGui::BeginCombo("Current Camera", names[cam_cur_id].c_str(), 0))
			{
				for (int n = 0; n < names.size(); ++n)
				{
					const bool is_selected = (cam_cur_id == n);
					if (ImGui::Selectable(names[n].c_str(), is_selected)) {
						auto old = cameras[cam_cur_id];
						auto new_one = cameras[n];
						cam_cur_id = n;
						ecs::registry.emplace<scn::renderable>(new_one);
						ecs::registry.remove<scn::renderable>(old);
					}

					// Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}

			for (const auto ent : ecs::registry.view<scn::camera_component, scn::renderable>()) {
				eng::transform3d ct{ glm::mat4{1.0} };
				if (ecs::registry.all_of<scn::local_transform>(ent)) {
					auto& trans = ecs::registry.get<scn::local_transform>(ent);
					ct = eng::transform3d{ trans.local };
				}
				ImGui::Text("pitch: %.3f, yaw: %.3f, roll: %.3f", glm::degrees(ct.get_pitch()), glm::degrees(ct.get_yaw()), glm::degrees(ct.get_roll()));
				auto pos = ct.get_pos();
				ImGui::Text("x: %.3f, y: %.3f, z: %.3f", pos.x, pos.y, pos.z);
			}
		}

		ImGui::Text("Scene");
		ImGui::Separator();

		static int model_load_method = 0;
		ImGui::RadioButton("Combo models", &model_load_method, 0); ImGui::SameLine();
		ImGui::RadioButton("Input model", &model_load_method, 1);

		gs::get_system().check_loaded_model();
		if (ImGui::Button("Add model")) {
			gs::get_system().load_model(model_load_method == 0 ? models_list[current_model].c_str() : buf);
		} ImGui::SameLine();

		if (model_load_method == 0) {
			if (ImGui::BeginCombo("Models", models_list[current_model].c_str()))
			{
				for (int n = 0; n < models_list.size(); n++)
				{
					const bool is_selected = (current_model == n);
					if (ImGui::Selectable(models_list[n].c_str(), is_selected))
						current_model = n;

					// Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		} else {
			ImGui::InputText("Name", buf, 64);
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
					ecs::registry.emplace<scn::mouse_controller_component>(selected_entity);
					// Remove mouse controller from the camera if it exists
					for (const auto ent : ecs::registry.view<scn::camera_component>()) {
						if (ecs::registry.all_of<scn::mouse_controller_component>(ent)) {
							ecs::registry.remove<scn::mouse_controller_component>(ent);
						}
					}
				} else {
					ecs::registry.remove<scn::mouse_controller_component>(selected_entity);
					// Re-add mouse controller to the camera if it was removed
					for (const auto ent : ecs::registry.view<scn::camera_component>()) {
						ecs::registry.emplace<scn::mouse_controller_component>(ent);
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

bool editor::EditorSystem::show_file_dialog()
{
	bool is_open = true;
	file_dialog.show("Import", &is_open);
    return is_open;
}

bool editor::EditorSystem::show_web()
{
	const bool cur_is_show = GUI_IS_ITEM_CHECKED("Editor/Draw web");
	if (!cur_is_show && cur_is_show != is_show_web) {
		ecs::remove_component<scn::renderable>(editor_web);
		//ecs::remove_component<scn::renderable>(sky);
	}
	else if (cur_is_show && cur_is_show != is_show_web){
		ecs::registry.emplace<scn::renderable>(editor_web);
		//ecs::add_component(sky, scn::renderable{});
	} 

	is_show_web = cur_is_show;
	return true;
}

bool editor::EditorSystem::show_scene()
{
	bool is_open = true;
	if (ImGui::Begin("Scene", &is_open))
	{
		ImVec2 contentRegionAvailable = ImGui::GetContentRegionAvail();
		ImVec2 pos = ImGui::GetCursorScreenPos();
		glm::mat4 viewMatrix{ 0 };
		glm::mat4 projMat{ 0 };
		for (const auto ent : ecs::registry.view<scn::camera_component>())
		{
			auto& camera = ecs::registry.get<scn::camera_component>(ent);
			camera.viewport.size = glm::ivec2(contentRegionAvailable.x, contentRegionAvailable.y);
			if (ecs::registry.all_of<scn::local_transform>(ent)) {
				auto& trans = ecs::registry.get<scn::local_transform>(ent);
				viewMatrix = glm::inverse(trans.local);
			}
			if (camera.viewport.size != glm::ivec2{ 0 }) {
				projMat = glm::perspective(glm::radians(camera.fov), (float)camera.viewport.size.x / (float)camera.viewport.size.y, camera.near_distance, camera.far_distance);
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

bool editor::EditorSystem::show_ecs_test()
{
	init_ecs_test();

	bool is_open = true;
	ImGui::SetNextWindowSize(ImVec2{ 400, 300 }, ImGuiCond_FirstUseEver);
	if (ImGui::Begin("ECS Test", &is_open))
	{
		for (const auto ent : ecs::registry.view<eng::transform3d, glm::vec2>()) {
			auto& trn = ecs::registry.get<eng::transform3d>(ent);
			auto& v = ecs::registry.get<glm::vec2>(ent);
			ImGui::Text("entity: %d, pitch: %.3f, yaw: %.3f, roll: %.3f", ent, trn.get_pitch(), trn.get_yaw(), trn.get_roll());
			auto pos = trn.get_pos();
			ImGui::Text("\t\tx: %.3f, y: %.3f, z: %.3f", pos.x, pos.y, pos.z);
			ImGui::Text("\t\tx: %.3f, y: %.3f", v.x, v.y);
		}

		ImGui::Separator();
		ImGui::NewLine();

		for (auto ent : ecs::registry.view<eng::transform3d>()) {
			auto& trn = ecs::registry.get<eng::transform3d>(ent);
			ImGui::Text("entity: %d, pitch: %.3f, yaw: %.3f, roll: %.3f", ent, trn.get_pitch(), trn.get_yaw(), trn.get_roll());
			auto pos = trn.get_pos();
			ImGui::Text("\t\tx: %.3f, y: %.3f, z: %.3f", pos.x, pos.y, pos.z);
		}

		ImGui::End();
	}

	return is_open;
}

bool editor::EditorSystem::show_materials()
{
	bool is_open = true;
	if (ImGui::Begin("Materials", &is_open, ImGuiWindowFlags_NoScrollbar))
	{
		auto mlts_view = ecs::registry.view<scn::base_material_component>();
		std::vector<ecs::entity> mlts;
		for (auto& ent : mlts_view) {
			mlts.push_back(ent);
		}

		auto items_getter = [](void* data, int idx) -> const char*
		{
			std::vector<ecs::entity>* items = (std::vector<ecs::entity>*)data;
			if (idx < 0 || idx >= items->size())
				return nullptr;
			if (ecs::registry.all_of<scn::name_component>((*items)[idx])) {
				auto& name = ecs::registry.get<scn::name_component>((*items)[idx]);
				return name.name.c_str();
			}

			return "Unnamed material";
		};
		ImVec2 windowSize = ImGui::GetContentRegionAvail();
		float itemHeight = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;

		int visibleItems = static_cast<int>((windowSize.y - ImGui::GetStyle().ItemSpacing.y) / itemHeight);

		static int item_current = 1;
		ImGui::PushItemWidth(windowSize.x);
		ImGui::ListBox("##materials_listbox", &item_current, items_getter, (void*)&mlts, mlts.size(), std::min(visibleItems, (int)mlts.size()));
		ImGui::PopItemWidth();
		if (item_current >= 0 && item_current < mlts.size())
		{
			ImGui::Begin("Material Properties");
			test_json_selected_material = mlts[item_current];
			if (ecs::registry.all_of<scn::base_material_component>(mlts[item_current])) {
				auto& material = ecs::registry.get<scn::base_material_component>(mlts[item_current]);
				ImGui::ColorEdit4("Albedo", &material.albedo.r);
				ImGui::ColorEdit4("Specular", &material.specular.r);
				ImGui::ColorEdit4("Ambient", &material.ambient.r);
				ImGui::ColorEdit4("Emissive", &material.emissive.r);
				ImGui::DragFloat("Shininess", &material.shininess, 1.0f, 0.0f, 1000.0f);
			}

			if (ecs::registry.all_of<scn::transparent_material_component>(mlts[item_current])) {
				auto& transparent = ecs::registry.get<scn::transparent_material_component>(mlts[item_current]);
				ImGui::ColorEdit4("Transparent", &transparent.transparent.r);
				ImGui::DragFloat("Opacity", &transparent.opacity, 0.01f, 0.0f, 1.0f);
			}

			if (ecs::registry.all_of<scn::reflective_material_component>(mlts[item_current])) {
				auto& reflective = ecs::registry.get<scn::reflective_material_component>(mlts[item_current]);
				ImGui::ColorEdit4("Reflective", &reflective.reflective.r);
				ImGui::DragFloat("Reflectivity", &reflective.reflectivity, 0.01f, 0.0f, 1.0f);
			}

			if (ecs::registry.all_of<scn::refractive_material_component>(mlts[item_current])) {
				auto& refractive = ecs::registry.get<scn::refractive_material_component>(mlts[item_current]);
				ImGui::DragFloat("Refraction Index", &refractive.refracti, 0.01f, 0.0f, 10.0f);
			}

			if (ecs::registry.all_of<scn::shininess_strength_component>(mlts[item_current])) {
				auto& shininess = ecs::registry.get<scn::shininess_strength_component>(mlts[item_current]);
				ImGui::DragFloat("Shininess Strength", &shininess.shininess_strength, 0.01f, 0.0f, 1.0f);
			}

			if (ecs::registry.all_of<scn::normal_map_component>(mlts[item_current])) {
				auto& normal_map = ecs::registry.get<scn::normal_map_component>(mlts[item_current]);
				ImGui::Text("Normal Map: %s", normal_map.txm.get_full().data());
			}

			if (ecs::registry.all_of<scn::metallic_map_component>(mlts[item_current])) {
				auto& metallic_map = ecs::registry.get<scn::metallic_map_component>(mlts[item_current]);
				ImGui::Text("Metallic Map: %s", metallic_map.txm.get_full().data());
			}

			if (ecs::registry.all_of<scn::roughness_map_component>(mlts[item_current])) {
				auto& roughness_map = ecs::registry.get<scn::roughness_map_component>(mlts[item_current]);
				ImGui::Text("Roughness Map: %s", roughness_map.txm.get_full().data());
			}

			if (ecs::registry.all_of<scn::ao_map_component>(mlts[item_current])) {
				auto& ao_map = ecs::registry.get<scn::ao_map_component>(mlts[item_current]);
				ImGui::Text("AO Map: %s", ao_map.txm.get_full().data());
			}

			if (ecs::registry.all_of<scn::albedo_map_component>(mlts[item_current])) {
				auto& albedo_map = ecs::registry.get<scn::albedo_map_component>(mlts[item_current]);
				ImGui::Text("Albedo Map: %s", albedo_map.txm.get_full().data());
			}

			if (ecs::registry.all_of<scn::is_transparent_flag_component>(mlts[item_current])) {
				if (ImGui::Button("-##transparent_flag")) {
					ecs::remove_component<scn::is_transparent_flag_component>(mlts[item_current]);
				}
				ImGui::SameLine();
				ImGui::Text("Transparent");
			} else {
				if (ImGui::Button("+##transparent_flag")) {
					ecs::registry.emplace<scn::is_transparent_flag_component>(mlts[item_current]);
				}
				ImGui::SameLine(); 
				ImGui::Text("Transparent");
			}

			ImGui::End();
		}	
	
	}
	ImGui::End();


	return is_open;
}

bool editor::EditorSystem::show_textures()
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
		ImGui::ListBox("##materials_listbox", &item_current, items_getter, (void*)&list, list.size(), std::min(visibleItems, (int)list.size()));
		ImGui::PopItemWidth();

		if (item_current >= 0 && item_current < list_tags.size())
		{
			glm::vec2 pos{ pos1.x + max_name * 10, pos1.y };
			glm::vec2 contentRegionAvailable{ windowSize.x - max_name * 10, windowSize.y };
			auto texture = rnd::get_system().get_texture_manager().find(list_tags[item_current]);
			auto* backend = wnd::get_system().get_gui_backend();

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
		}
	}
	ImGui::End();

	return is_open;
}

bool editor::EditorSystem::show_clear_cache()
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

void editor::EditorSystem::draw_manipulator(const glm::vec2& pos, const glm::vec2& size)
{

}

void editor::EditorSystem::draw_gizmo(const glm::vec2& start, const glm::vec2& size, const glm::mat4& view, const glm::mat4& proj)
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

void editor::EditorSystem::draw_scene_image(const glm::vec2& pos, const glm::vec2& contentRegionAvailable)
{
	auto texture = rnd::get_system().get_texture_manager().find(res::tag(res::tag::memory, "__color_scene_rt"));
	auto* backend = wnd::get_system().get_gui_backend();

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

	style.Colors[ImGuiCol_Button] = original_button_color;
	style.Colors[ImGuiCol_ButtonHovered] = original_button_hovered_color;
	style.Colors[ImGuiCol_ButtonActive] = original_button_active_color;
	style.FramePadding = original_padding;
	// let input to go to game
	if (ImGui::IsItemHovered()) {
		input->unblock_layer_once();
	}
}

bool editor::EditorSystem::init_ecs_test()
{
	if (is_inited_ecs_test) {
		return true;
	}

	is_inited_ecs_test = true;
	ecs::entity first = ecs::create_entity();
	eng::transform3d trn{ glm::translate(glm::mat4{1.0}, glm::vec3(6)) };
	ecs::registry.emplace<eng::transform3d>(first, trn);
	glm::vec2 tmp{ 9, 9 };
	ecs::registry.emplace<glm::vec2>(first, tmp);
	trn.set_pos(glm::vec3{ 6, 6, 6 });
	ecs::entity second = ecs::create_entity();
	eng::transform3d trn2{ glm::translate(glm::mat4{1.0}, glm::vec3(3)) };
	ecs::registry.emplace<eng::transform3d>(second, trn2);
	trn2.add_yaw(45.f);

	return true;
}
