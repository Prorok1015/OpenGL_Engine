#include "gs_game_system.h"

#include <inp_input_manager.h>
#include <inp_input_system.h>

#include <enums.h>
#include <scn_primitives.h>
#include <scn_model.h>

#include <wnd_window_system.h>
#include "res_system.h"

#include <rnd_render_system.h>
#include <ecs_common_system.h>

#include <timer.hpp>
#include <glm/gtc/random.hpp>
#include "ecs_system.h"
#include "scn_camera_component.hpp"
#include "scn_transform_system.h"
#include "scn_animation_job.h"
#include "scn_material_component.hpp"
#include "ecs_component.h"

gs::GameSystem* p_game_system = nullptr;
extern int gMaxTexture2DSize;

scn::mouse_controller_job mouse_controller_system;
scn::transform_job transform_job_instance;
scn::animation_job animation_job_instance;

gs::GameSystem& gs::get_system()
{
	ASSERT_MSG(p_game_system, "Game system is nullptr!");
	return *p_game_system;
}

template<typename R>
bool is_ready(std::future<R> const& f)
{
	return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

gs::GameSystem::GameSystem()
{
	window = wnd::get_system().get_active_window();
	//input = std::make_shared<inp::input_manager>();
	ecs_input = std::make_shared<ecs::flow_input_manager>();
	//inp::get_system().activate_manager(input);
	inp::get_system().activate_manager(ecs_input);

	renderer = std::make_shared<scn::renderer_3d>();
	rnd::get_system().activate_renderer(renderer);
}

gs::GameSystem::~GameSystem()
{
	rnd::get_system().deactivate_renderer(renderer);
}

void gs::GameSystem::set_enable_input(bool enable)
{
}

void gs::GameSystem::load_model(std::string_view path)
{
	future_model = std::make_shared<std::future<std::shared_ptr<res::Model>>>(res::get_system().require_resource_async<res::Model>(res::Tag::make(path)));
}

void ensure_ecs_material(ecs::entity material, const res::Material& mlt)
{
	ecs::registry.emplace<scn::name_component>(material, mlt.name);
	scn::base_material_component base_mlt;
	if (mlt.is_state(res::Material::ALBEDO_COLOR)) {
		base_mlt.albedo = mlt.diffuse_color;
	}
	if (mlt.is_state(res::Material::SPECULAR_COLOR)) {
		base_mlt.specular = mlt.specular_color;
	}
	if (mlt.is_state(res::Material::AMBIENT_COLOR)) {
		base_mlt.ambient = mlt.ambient_color;
	}
	if (mlt.is_state(res::Material::EMISSIVE_COLOR)) {
		base_mlt.emissive = mlt.emissive_color;
	}
	if (mlt.is_state(res::Material::SHININESS)) {
		base_mlt.shininess = mlt.shininess;
	}

	if (mlt.is_state(res::Material::TRANSPARENT_COLOR)) {
		scn::transparent_material_component transparent;
		transparent.transparent = mlt.transparent_color;
		if (mlt.is_state(res::Material::OPACITY)) {
			transparent.opacity = mlt.opacity;
		}
		ecs::registry.emplace<scn::transparent_material_component>(material, transparent);
	}

	if (mlt.is_state(res::Material::REFLECTIVE_COLOR)) {
		scn::reflective_material_component reflective;
		reflective.reflective = mlt.reflective_color;
		if (mlt.is_state(res::Material::REFLECTIVITY)) {
			reflective.reflectivity = mlt.reflectivity;
		}
		ecs::registry.emplace<scn::reflective_material_component>(material, reflective);
	}

	if (mlt.is_state(res::Material::REFRACTI)) {
		ecs::registry.emplace<scn::refractive_material_component>(material, mlt.refracti);
	}

	if (mlt.is_state(res::Material::SHININESS_STRENGTH)) {
		ecs::registry.emplace<scn::shininess_strength_component>(material, mlt.shininess_strength);
	}

	ecs::registry.emplace<scn::base_material_component>(material, base_mlt);
	if (mlt.is_state(res::Material::ALBEDO_TXM)) {
		ecs::registry.emplace<scn::albedo_map_component>(material, mlt.get_txm(res::Material::ALBEDO_TXM));
	}
	if (mlt.is_state(res::Material::NORMALS_TXM)) {
		ecs::registry.emplace<scn::normal_map_component>(material, mlt.get_txm(res::Material::NORMALS_TXM));
	}
	if (mlt.is_state(res::Material::SPECULAR_TXM)) {
		ecs::registry.emplace<scn::specular_map_component>(material, mlt.get_txm(res::Material::SPECULAR_TXM));
	}
	if (mlt.is_state(res::Material::AMBIENT_OCCLUSION_TXM)) {
		ecs::registry.emplace<scn::ao_map_component>(material, mlt.get_txm(res::Material::AMBIENT_OCCLUSION_TXM));
	}
}

void ensure_ecs_node(ecs::entity ent, const res::node_hierarchy_view& node, const res::meshes_conteiner& data, std::unordered_map<int, ecs::entity> material_mapping)
{
	ecs::registry.emplace<scn::renderable>(ent);
	ecs::registry.emplace<scn::name_component>(ent, node.name);
	ecs::registry.emplace_or_replace<scn::local_transform>(ent, node.mt);

	if (!node.anim.empty()) {
		ecs::registry.emplace<scn::keyframes_component>(ent, node.anim);
	}

	if (node.bone_id != -1) {
		auto& bone = data.bones[node.bone_id];
		ecs::registry.emplace<scn::bone_component>(ent, bone.offset);
		if (ecs::registry.all_of<scn::keyframes_component>(ent)) {
			auto& key = ecs::registry.get<scn::keyframes_component>(ent);
			key.keyframes = bone.anim;
			egLOG("model/fill_components", "There is double animation on the node '{0}'", node.name);
		} else {
			ecs::registry.emplace<scn::keyframes_component>(ent, bone.anim);
		}
	}

	std::vector<ecs::entity> children;
	for (auto& mesh : node.meshes)
	{
		ecs::entity mesh_ent = ecs::create_entity();
		children.push_back(mesh_ent);
		ecs::registry.emplace<scn::parent_component>(mesh_ent, ent);
		ecs::registry.emplace<scn::mesh_component>(mesh_ent, mesh);
		ecs::registry.emplace<scn::local_transform>(mesh_ent);
		ecs::registry.emplace<scn::world_transform>(mesh_ent);
		ecs::registry.emplace<scn::renderable>(mesh_ent);
		ecs::registry.emplace<scn::material_link_component>(mesh_ent, material_mapping[mesh.material_id]);
	}

	for (auto& child : node.children)
	{
		ecs::entity child_ent = ecs::create_entity();
		children.push_back(child_ent);
		ecs::registry.emplace<scn::parent_component>(child_ent, ent);
		ensure_ecs_node(child_ent, child, data, material_mapping);
	}

	if (!children.empty()) {
		ecs::registry.emplace<scn::children_component>(ent, children);
	}
}

// TODO: remove
void gs::GameSystem::end_ecs_frame()
{
	const auto ents = ecs::registry.view<ecs::input_changed_event_component>();
	ecs::registry.destroy(ents.begin(), ents.end());

	inp::get_system().mouse.clear_scroll();
}

void gs::GameSystem::check_loaded_model()
{
	if (future_model && is_ready(*future_model)) {
		auto res = future_model->get();
		auto& pres = res->get_model_pres().data;
		auto& root = res->get_model_pres();
		auto rand_pos = glm::ballRand(20.f);
		auto model = glm::translate(glm::mat4(1.0), glm::vec3{ 0 });
		model = glm::scale(model, glm::vec3{ 1 });

		ecs::entity obj = ecs::create_entity();
		if (auto& bones_data = pres.bones_data.bones_indeces; !bones_data.empty()) { 
			// TODO: create texture only for model desc
			res::Tag txm = res::Tag("memory", "__bones_indeces_" + std::to_string((int)obj));
			auto& last_bone_view = pres.bones_data;
			last_bone_view.bones_indeces_txm = txm;

			glm::ivec2 size{ pres.vertices.size(), 0 };
			size.y = bones_data.size() / size.x;

			const auto& origin_width = size.x;
			const auto& origin_height = size.y;
			last_bone_view.original_size = { 1, origin_height };

			if (origin_width > gMaxTexture2DSize)
			{
				int real_width = origin_width;
				int pices_count = 1;
				while (real_width > gMaxTexture2DSize)
				{
					if (real_width % 2 != 0) {
						++real_width;
					}
					real_width /= 2;
					pices_count *= 2;
				}

				int add = (real_width * pices_count) - origin_width;

				bones_data.reserve(bones_data.size() + (add * origin_height));
				for (int i = 0; i < add; ++i)
				{
					for (int j = 1; j <= origin_height; ++j) {
						int offset = j * origin_width;
						bones_data.insert(bones_data.begin() + offset, -1);
					}
				}

				last_bone_view.original_size = { pices_count, origin_height };
				size.x = real_width;
				size.y = bones_data.size() / real_width;
			}
			

			rnd::driver::texture_header header;
			header.data.initial_data = (unsigned char*)bones_data.data();
			header.data.format = rnd::driver::texture_header::TYPE::R32I;

			header.data.extent.width = size.x;
			header.data.extent.height = size.y;

			header.wrap = rnd::driver::texture_header::WRAPPING::CLAMP_TO_EDGE;
			header.min = rnd::driver::texture_header::FILTERING::NEAREST;
			header.mag = rnd::driver::texture_header::FILTERING::NEAREST;

			rnd::get_system().get_texture_manager().generate_texture(txm, header);
		}

		root.head.mt = model * root.head.mt;
		std::unordered_map<int, ecs::entity> material_mapping;
		auto anchors = ecs::registry.view<scn::scene_anchor_component>();
		ecs::entity world_anchor;

		if (anchors.empty()) {
			world_anchor = ecs::create_entity();
			ecs::registry.emplace<scn::scene_anchor_component>(world_anchor);
			ecs::registry.emplace<scn::name_component>(world_anchor, "Anchor");
		} else {
			world_anchor = anchors.front();
		}
		decltype(scn::children_component::children) children;
		if (ecs::registry.all_of<scn::children_component>(world_anchor)) {
			auto& kids = ecs::registry.get<scn::children_component>(world_anchor);
			children = kids.children;
			ecs::remove_component<scn::children_component>(world_anchor);
		}

		for (int i = 0; i < pres.materials.size(); ++i) {
			auto material = ecs::create_entity();
			ensure_ecs_material(material, pres.materials[i]);
			material_mapping[i] = material;
		}

		children.push_back(obj);
		ecs::registry.emplace<scn::children_component>(world_anchor, children);

		ecs::registry.emplace<scn::parent_component>(obj, world_anchor);
		ecs::registry.emplace<scn::model_root_component>(obj, pres, res->get_tag());
		if (!root.animations.empty()) {
			ecs::registry.emplace<scn::animations_component>(obj, root.animations);
		}		


		ensure_ecs_node(obj, root.head, pres, material_mapping);


		renderer->scene_objects.push_back(obj);
		future_model = nullptr;
	}
}

void gs::GameSystem::reload_shaders()
{
	rnd::get_system().get_shader_manager().clear_cache();
}

void gs::GameSystem::add_cube_to_scene(float radius)
{
	glm::vec2 rand_pos = glm::diskRand(radius);
	auto& inst = ecs::registry.get<res::instance_object>(cubes_inst);
	inst.worlds.push_back(glm::translate(glm::mat4{ 1.0 }, glm::vec3{ rand_pos.x, 1.f, rand_pos.y }));

	//static bool is_gen_cube = true;
	//scn::Model m = is_gen_cube ? generate_cube() : generate_sphere();
	//is_gen_cube = !is_gen_cube;
	//ecs::entity obj = ecs::create_entity();
	//ecs::add_component(obj, scn::model_comonent{ m.meshes });
	//ecs::add_component(obj, scn::transform_component{ m.model });
	//ecs::add_component(obj, scn::renderable{});

	//renderer->scene_objects.push_back(obj);
}

void gs::GameSystem::remove_cube()
{
	if (renderer->scene_objects.empty()) {
		return;
	}

	ecs::remove_entity(renderer->scene_objects.back());

	renderer->scene_objects.pop_back();
}
