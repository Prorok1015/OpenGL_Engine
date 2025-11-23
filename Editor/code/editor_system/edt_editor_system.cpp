#include "edt_editor_system.h"
#include "gui_api.hpp"
#include "application.h"
#include "gs_game_system.h"
#include "res_system.h"
#include "resources/res_resource_picture.h"
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
#include "geom/rnd_geometry_desc.h"
#include "texture/rnd_texture_desc.h"
#include "scn_material_desc.h"
#include "scn_skinning_prototype_desc.h"
#include "adapters/scn_model_importer_adapter.h"

#include "common/ds_svg_writer.hpp"

namespace ds {

	point2d center(const bbox& box) { return (box.min + box.max) * 0.5f; }
	void expand(bbox& box, const bbox& other) {
		box.min = glm::min(box.min, other.min);
		box.max = glm::max(box.max, other.max);
	}
	void expand(bbox& box, const point2d& p) {
		box.min = glm::min(box.min, p);
		box.max = glm::max(box.max, p);
	}
	bool intersects(const bbox& a, const bbox& b) {
		return (a.min.x <= b.max.x && a.max.x >= b.min.x) && (a.min.y <= b.max.y && a.max.y >= b.min.y);
	}
	bool contains(const bbox& a, const point2d& p) {
		return p.x >= a.min.x && p.x <= a.max.x && p.y >= a.min.y && p.y <= a.max.y;
	}

}

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

editor::editor_system::editor_system(desc::desc_system& desc_system_)
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
			if (selected_entity != entt::null) 
			{
				if (auto* desc = ecs::registry.try_get<rnd::geometry_desc>(selected_entity))
				{
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


			//ImGui::Text("This is a test window for JSON serialization.");
			//if (test_json_selected_material != entt::null) {
			//	auto obj = scn::convert_material_to_json(test_json_selected_material); 
			//	std::ostringstream oss;
			//	pretty_print(oss, obj);
			//	std::string json_string = oss.str();
			//	ImGui::InputTextMultiline("JSON", &json_string, ImVec2(500, 500), ImGuiInputTextFlags_ReadOnly); 
			//}

			ImGui::End();
		}
		return is_open;
	});
	GUI_SET_ITEM_CHECKED("Editor/Test JSON Window", false);

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


	auto logo = res::get_system().require_resource<res::picture_resource>(res::tag::make("icons/editor_engine_logo.png"));
	wnd::get_system().get_active_window()->set_logo(logo);
	wnd::get_system().get_active_window()->set_title("Snake Editor");
	gui::get_system().set_show_title_bar(true);
	gui::get_system().set_show_title_bar_dbg(true);

	input = std::make_shared<edt::input_manager>();
	input->unblock_layer_once();
	inp::get_system().activate_manager(input);

	file_dialog.set_current_path(res::get_system().get_resources_path());

	auto txt = rnd::get_system().get_texture_manager().generate_texture(res::tag(res::tag::memory, "__black"), {1,1}, rnd::driver::texture_header::TYPE::RGB8, {0, 0, 0});
	auto txt2 = rnd::get_system().get_texture_manager().generate_texture(res::tag(res::tag::memory, "__red"), {1,1}, rnd::driver::texture_header::TYPE::RGB8, {255, 0, 0});

	auto anchors = ecs::registry.view<scn::scene_anchor_component>();
	ecs::entity world_anchor;
	if (anchors.empty()) {
		world_anchor = ecs::registry.create();
		ecs::registry.emplace<scn::scene_anchor_component>(world_anchor);
		ecs::registry.emplace<scn::name_component>(world_anchor, scn::name_component{ .name = "Anchor" });
	}
	else {
		world_anchor = anchors.front();
	}

	decltype(scn::children_component::children)& children = ecs::registry.get_or_emplace<scn::children_component>(world_anchor).children;

	if (false)
	{
		rnd::texture_desc txm_desc;
		txm_desc.txm_name = "window";
		txm_desc.txm_tag = res::tag::make("window.png");
		auto& header = txm_desc.header;
		header.data.format = rnd::driver::texture_header::TYPE::RGBA8;
		header.data.extent.width = 4587;
		header.data.extent.height = 8000;

		json::object txm_js;
		txm_desc.serialize(txm_js);
		std::string str_data = json::serialize(txm_js);
		std::vector<std::byte> txm_data;
		txm_data.resize(str_data.size());
		std::memcpy(txm_data.data(), str_data.data(), str_data.size());
		res::get_system().memory_resolver_.add_memory_resource(res::tag(res::tag::memory, "window.desc"), txm_data);

		desc_system.register_desc<rnd::texture_desc>(res::tag(res::tag::memory, "window.desc"), std::string{ txm_desc.txm_tag.pure_name() });
	
		res::tag window_material = res::tag(res::tag::memory, "window_material.desc");
		scn::material_desc mlt;

		rnd::shader_config::constant_data tmpdesc;

		tmpdesc.program = rnd::shader_config::shader_program_data::build()
			.set_vertex_shader(res::tag::make("shaders/mix_opaque_trans_scene.vert"))
			.set_fragment_shader(res::tag::make("shaders/mix_opaque_trans_scene.frag"));

		tmpdesc.defines = { "USE_TXM_AS_DIFFUSE", "LIGHTS_ENABLED" };
		mlt.queue = scn::pass_queue::MIX;
		mlt.cdata = tmpdesc;
		mlt.samplers_textures_desc = { desc_system.get_desc<rnd::texture_desc>(res::tag(res::tag::memory, "window.desc")) };

		json::object mlt_js;
		mlt.serialize( mlt_js);
		std::string str_data2 = json::serialize(mlt_js);
		std::vector<std::byte> mlt_data;
		mlt_data.resize(str_data2.size());
		std::memcpy(mlt_data.data(), str_data2.data(), str_data2.size());
		res::get_system().memory_resolver_.add_memory_resource(window_material, mlt_data);

		desc_system.register_desc<scn::material_desc>(window_material, std::string{ window_material.pure_name() });



	}

	{
		scn::material_desc mlt;

		mlt.cdata.program = rnd::shader_config::shader_program_data::build()
			.set_vertex_shader(res::tag::make("shaders/scene.vert"))
			.set_fragment_shader(res::tag::make("shaders/scene.frag"));

		mlt.cdata.defines;
		mlt.queue = scn::pass_queue::OPAQUE;
		mlt.render_mode = rnd::driver::RENDER_MODE::LINE;
		mlt.albedo = ds::color(glm::vec3(0.f), 1.0f);

		json::object mlt_js;
		mlt.serialize(mlt_js);
		std::string str_data2 = json::serialize(mlt_js);
		std::vector<std::byte> mlt_data;
		mlt_data.resize(str_data2.size());
		std::memcpy(mlt_data.data(), str_data2.data(), str_data2.size());

		res::tag web_material = res::tag(res::tag::memory, "web_material.desc");
		res::get_system().memory_resolver_.add_memory_resource(web_material, mlt_data);

		desc_system.register_desc<scn::material_desc>(web_material, std::string{ web_material.pure_name() });

	}

	//  camera
	{
		glm::ivec4 viewport{ glm::zero<glm::ivec2>(), wnd::get_system().get_active_window()->get_size() };
		glm::vec3 rotation(0);
		rotation.x = -glm::radians(45.0f);
		auto ecs_entity = ecs::registry.create();
		children.push_back(ecs_entity);
		ecs::registry.emplace<scn::name_component>(ecs_entity, "Editor camera");
		ecs::registry.emplace<scn::parent_component>(ecs_entity, world_anchor);
		ecs::registry.emplace<scn::camera_component>(ecs_entity, scn::camera_component{ .viewport = viewport });
		ecs::registry.emplace<scn::local_transform>(ecs_entity);
		ecs::registry.emplace<scn::world_transform>(ecs_entity);
		ecs::registry.emplace<scn::renderable>(ecs_entity);
		ecs::registry.emplace<scn::mouse_controller_component>(ecs_entity, scn::mouse_controller_component{ .rotation = rotation });
	}


	// web
	if (false)
	{	
		auto web = scn::generate_web({ 50, 50 });
		res::tag web_tag = res::tag(res::tag::memory, "web.desc");
		rnd::geometry_desc geom_desc;
		geom_desc.layout = {
			{rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position"},
			{rnd::driver::SHADER_DATA_TYPE::VEC3_F, "normal"},
			{rnd::driver::SHADER_DATA_TYPE::VEC2_F, "uv"},
		};

		geom_desc.indices = web.indices;
		geom_desc.vertices.resize(web.vertices.size() * sizeof(scn::vertex));
		std::memcpy(geom_desc.vertices.data(), (std::byte*)web.vertices.data(), geom_desc.vertices.size());
		auto geom_tag = web_tag;
		json::object geom_js;
		geom_desc.serialize(geom_js);

		std::string str_data = json::serialize(geom_js);
		std::vector<std::byte> geom_data;
		geom_data.resize(str_data.size());
		std::memcpy(geom_data.data(), str_data.data(), str_data.size());

		res::get_system().memory_resolver_.add_memory_resource(geom_tag, geom_data);

		// 2. register new geometry_desc
		desc_system.register_desc<rnd::geometry_desc>(geom_tag, std::string{ geom_tag.pure_name() });

		scn::prototype_desc web_prototype_desc;
		web_prototype_desc.geometry = desc_system.get_desc<rnd::geometry_desc>(geom_tag);
		web_prototype_desc.root.name = "Editor Web";
		web_prototype_desc.root.local = glm::scale(glm::vec3(1, 0, 1));
		web_prototype_desc.root.mesh = scn::prototype_desc::mesh_t{
			.vx_begin = 0, .vx_end = web.vertices.size(),
			.ind_begin = 0, .ind_end = web.indices.size(),
			.material = desc_system.get_desc<scn::material_desc>(res::tag(res::tag::memory, "web_material.desc"))
		};

		web_prototype_desc.load_prototype(ecs::registry, world_anchor);
	}
	
	if (false)
	{

		auto geom = scn::generate_cube();
		res::tag cube_tag = res::tag(res::tag::memory, "cube.desc");
		// windows objects
		for (int i = 0; i < 1; ++i)
		{
			auto wind = ecs::registry.create();
			children.push_back(wind);

			rnd::geometry_desc geom_desc;
			geom_desc.layout = {
				{rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position"},
				{rnd::driver::SHADER_DATA_TYPE::VEC3_F, "normal"},
				{rnd::driver::SHADER_DATA_TYPE::VEC2_F, "texture_position"}
			};

			geom_desc.indices = geom.indices;
			geom_desc.vertices.resize(geom.vertices.size() * sizeof(scn::vertex));
			std::memcpy(geom_desc.vertices.data(), (std::byte*)geom.vertices.data(), geom_desc.vertices.size());
			auto geom_tag = cube_tag;
			json::object geom_js;
			geom_desc.serialize(geom_js);

			std::string str_data = json::serialize(geom_js);
			std::vector<std::byte> geom_data;
			geom_data.resize(str_data.size());
			std::memcpy(geom_data.data(), str_data.data(), str_data.size());

			res::get_system().memory_resolver_.add_memory_resource(geom_tag, geom_data);

			// 2. register new geometry_desc
			desc_system.register_desc<rnd::geometry_desc>(geom_tag, std::string{ geom_tag.pure_name() });

			glm::vec2 rnd_pos = glm::diskRand(1.f);
			scn::prototype_desc window_prototype_desc;
			window_prototype_desc.geometry = desc_system.get_desc<rnd::geometry_desc>(geom_tag);
			window_prototype_desc.root.name = "Window";
			window_prototype_desc.root.local = glm::translate(glm::mat4{ 1.0 }, glm::vec3(rnd_pos.x, 0, rnd_pos.y));
			window_prototype_desc.root.mesh = scn::prototype_desc::mesh_t{
				.vx_begin = 0, .vx_end = geom.vertices.size(),
				.ind_begin = 0, .ind_end = geom.indices.size(),
				.material = desc_system.get_desc<scn::material_desc>(res::tag(res::tag::memory, "window_material.desc"))
			};
			window_prototype_desc.root.children = { window_prototype_desc.root };
			window_prototype_desc.root.children[0].name = "Window2";
			window_prototype_desc.root.children[0].local = glm::translate(glm::mat4{ 1.0 }, glm::vec3(rnd_pos.x + 3, 0, rnd_pos.y + 3));

			window_prototype_desc.load_prototype(ecs::registry, world_anchor);
		}
	}

	// light
	{
		light = ecs::registry.create();
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
		sky = ecs::registry.create();
		children.push_back(sky);
		ecs::registry.emplace<scn::sky_component>(sky, scn::sky_component{ .cube_map = std::vector<res::tag>{
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
	this->world_anchor = world_anchor;
	res::get_system().registrate_adapter(scn::model_importer_adapter::INFO, scn::model_importer_adapter{desc_system});

	desc_system.register_desc<scn::material_desc>(res::tag::make("base_material.desc"));
	desc_system.register_desc<rnd::geometry_desc>(res::tag::make("base_geometry.desc"));
	desc_system.register_desc<rnd::texture_desc>(res::tag::make("base_texture.desc"), "base");

	desc_system.register_desc<scn::skinning_prototype_desc>(res::tag::make("objects/fsb/scene.gltf"), "backpack");
	imported_models_list.push_back(res::tag::make("objects/fsb/scene.gltf"));
	backpack = desc_system.get_desc<scn::skinning_prototype_desc>("backpack");
}

editor::editor_system::~editor_system()
{
	inp::get_system().deactivate_manager(input);
}

void mark_node_for_animation_stop(entt::entity ent)
{
	if (ecs::registry.all_of<scn::keyframes_component>(ent)) {
		ecs::registry.remove<scn::playable_animation_component>(ent);
	}

	if (auto* children = ecs::registry.try_get<scn::children_component>(ent)) {
		for (auto& child : children->children) {
			mark_node_for_animation_stop(child);
		}
	}
}

void mark_node_for_animation(entt::entity ent, const res::animation& animation)
{
	if (ecs::registry.all_of<scn::keyframes_component>(ent)) {
		scn::playable_animation_component tmp{ animation.name, animation.duration, animation.ticks_per_second };
		ecs::registry.emplace_or_replace<scn::playable_animation_component>(ent, std::move(tmp));
	}

	if (auto* children = ecs::registry.try_get<scn::children_component>(ent)) {
		for (auto& child : children->children) {
			mark_node_for_animation(child, animation);
		}
	}
}

void editor::editor_system::show_tree_items(ecs::entity ent)
{
	std::string obj_idx = std::to_string((int)ent);
	std::string name = "Node##" + obj_idx;
	if (ecs::registry.all_of<scn::name_component>(ent)) {
		auto& com_name = ecs::registry.get<scn::name_component>(ent);
		if (!com_name.name.empty()) {
			name = (ecs::registry.all_of<scn::mesh_component>(ent) ? "[MESH] " : "") + com_name.name + "##" + obj_idx;
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
					ecs::entity child = ecs::registry.create();
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
						ecs::registry.remove<scn::renderable>(ent);
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

		struct edt_playable_animation {
			std::string name;
			int idx = -1;
		};

		if (ecs::registry.all_of<scn::animations_component>(ent)) {
			auto& anims = ecs::registry.get<scn::animations_component>(ent);
			std::string_view play_anim_name;

			if (ecs::registry.all_of<edt_playable_animation>(ent)) {
				auto& play = ecs::registry.get<edt_playable_animation>(ent);
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
							if (ecs::registry.all_of<edt_playable_animation>(ent)) {
								ecs::registry.remove<edt_playable_animation>(ent);
								mark_node_for_animation_stop(ent);
							}
						} else {
							ecs::registry.emplace_or_replace<edt_playable_animation>(ent, edt_playable_animation{ .name = names[cur], .idx = cur });
							if (old != cur) {
								mark_node_for_animation(ent, anims.animations[cur - 1]);
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

bool editor::editor_system::show_toolbar()
{
	ImGuiIO& io = ImGui::GetIO();

	static bool is_last_frame_loaded = false;
	if (is_last_frame_loaded)
	{
		static std::once_flag f;
		std::call_once(f, [&]() {
			backpackent = backpack->load_prototype(ecs::registry, world_anchor);
			auto geom = backpack->geometry;
			int uvs_offset = 0;
			for (const auto& elem : geom->layout.get_elements())
			{
				if (elem.Name == "uv") {
					uvs_offset = elem.Offset;
					break;
				}
			}
			ds::svg_writer svg_writer("rtree_visualization.svg", 2048, 2048);

			std::vector<std::pair<ds::bbox, ds::triangle>> items;
			int stride = geom->layout.get_stride();
			for (int i = 0, triangle = 0; i < geom->indices.size(); i += 3)
			{
				uint32_t i0 = geom->indices[i + 0];
				uint32_t i1 = geom->indices[i + 1];
				uint32_t i2 = geom->indices[i + 2];
				auto &v0 = *(glm::vec2*)(geom->vertices.data() + i0 * stride + uvs_offset);
				auto &v1 = *(glm::vec2*)(geom->vertices.data() + i1 * stride + uvs_offset);
				auto &v2 = *(glm::vec2*)(geom->vertices.data() + i2 * stride + uvs_offset);
				ds::bbox box;
				expand(box, v0);
				expand(box, v1);
				expand(box, v2);
				items.push_back({ box, triangle++ });
				svg_writer.add_rect(box, triangle % 2 ? "blue" : "red");
			}

			rtree = geom->rtree;

			ds::svg_writer query_svg("rtree_query_visualization.svg", 2048, 2048);
			for (auto& v : rtree.data) {
				query_svg.add_rect(v.box, v.is_leaf ? "green" : "red");
			}

		});
	}

	is_last_frame_loaded = backpack && backpack->is_loaded();

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

		ImGui::Text("Import model");
		static int selected_model_idx = 0;
		if (!imported_models_list.empty()) {
			std::string buf = std::string{ imported_models_list[selected_model_idx].get_full() };
			if (ImGui::BeginCombo("##imported_objects", buf.c_str(), 0))
			{
				for (int n = 0; n < imported_models_list.size(); ++n)
				{
					const bool is_selected = (selected_model_idx == n);
					buf = std::string{ imported_models_list[n].get_full() };
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

		if (ImGui::Button("Create object on scene")) {
			auto robot = desc_system.get_desc<scn::prototype_desc>(imported_models_list[selected_model_idx]);
			if (robot) {
				robot->load_prototype(ecs::registry, world_anchor);
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

bool editor::editor_system::show_file_dialog()
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
			imported_models_list.push_back(tag);
			desc_system.register_desc<scn::skinning_prototype_desc>(tag, std::string{ tag.pure_name() });
		}
	}
    return is_open;
}

bool editor::editor_system::show_web()
{
	const bool cur_is_show = GUI_IS_ITEM_CHECKED("Editor/Draw web");
	if (!cur_is_show && cur_is_show != is_show_web) {
		ecs::registry.remove<scn::renderable>(editor_web);
		//ecs::remove_component<scn::renderable>(sky);
	}
	else if (cur_is_show && cur_is_show != is_show_web){
		ecs::registry.emplace<scn::renderable>(editor_web);
		//ecs::add_component(sky, scn::renderable{});
	} 

	is_show_web = cur_is_show;
	return true;
}

bool editor::editor_system::show_scene()
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

bool editor::editor_system::show_ecs_test()
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

bool editor::editor_system::show_materials()
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
					ecs::registry.remove<scn::is_transparent_flag_component>(mlts[item_current]);
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

void recurcive_set(const std::vector<uint32_t>& res, entt::entity ent, const ds::color& color, bool reset = true) {
	if (ecs::registry.all_of<scn::material_desc_component, scn::mesh_component>(ent)) {
		auto mesh = ecs::registry.get<scn::mesh_component>(ent);
		auto& hightlight = ecs::registry.get_or_emplace<scn::hightlight_component>(ent);
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

	if (ecs::registry.all_of<scn::children_component>(ent)) {
		auto children = ecs::registry.get<scn::children_component>(ent);
		for (const auto& child : children.children) {
			recurcive_set(res, child, color, reset);
		}
	}
}

bool editor::editor_system::show_textures()
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
					res = rtree.query(mouse_pos);
				}

				for (const auto& triangle : res) {
					std::cout << "Triangle: " << triangle << std::endl;
				}

				ecs::registry.clear<scn::hightlight_component>();
				recurcive_set(res, backpackent, picker_color);
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
					ds::scoped_timer timer("rtree rect query time");
					res = rtree.query(rect);
				}

				/*for (const auto& triangle : res) {
					std::cout << "Triangle in rect: " << triangle << std::endl;
				}*/
				ecs::registry.clear<scn::hightlight_component>();
				recurcive_set(res, backpackent, picker_color);
				is_dragging = false;
				is_hightlight_color_changed = false;
				rect = ds::bbox{};
			}

			if (is_hightlight_color_changed) {
				recurcive_set({}, backpackent, picker_color, false);
			}

		}
	}
	ImGui::End();

	return is_open;
}

bool editor::editor_system::show_clear_cache()
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

void editor::editor_system::draw_manipulator(const glm::vec2& pos, const glm::vec2& size)
{

}

void editor::editor_system::draw_gizmo(const glm::vec2& start, const glm::vec2& size, const glm::mat4& view, const glm::mat4& proj)
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

void editor::editor_system::draw_scene_image(const glm::vec2& pos, const glm::vec2& contentRegionAvailable)
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

bool editor::editor_system::init_ecs_test()
{
	if (is_inited_ecs_test) {
		return true;
	}

	is_inited_ecs_test = true;
	ecs::entity first = ecs::registry.create();
	eng::transform3d trn{ glm::translate(glm::mat4{1.0}, glm::vec3(6)) };
	ecs::registry.emplace<eng::transform3d>(first, trn);
	glm::vec2 tmp{ 9, 9 };
	ecs::registry.emplace<glm::vec2>(first, tmp);
	trn.set_pos(glm::vec3{ 6, 6, 6 });
	ecs::entity second = ecs::registry.create();
	eng::transform3d trn2{ glm::translate(glm::mat4{1.0}, glm::vec3(3)) };
	ecs::registry.emplace<eng::transform3d>(second, trn2);
	trn2.add_yaw(45.f);

	return true;
}
