#pragma once
#include "rnd_renderer_base.h"
#include "scn_model.h"
#include "scn_camera_component.hpp"
#include "ecs_entity.h"
#include "res_resource_model.h"
#include "shader/rnd_scene_shader_desc.h"

namespace rnd::driver
{
	class driver_interface;
	class vertex_array_interface;
	class buffer_interface;
}

namespace scn
{
	class renderer_3d : public rnd::renderer_base
	{
	public:
		renderer_3d();
		virtual ~renderer_3d() override;

		virtual void on_render(rnd::driver::driver_interface* drv);

		void draw(rnd::shader_scene_desc& desc, res::mesh_view& mesh, const rnd::driver::vertex_array_interface* va, rnd::driver::driver_interface* drv);
		void draw_instances(rnd::driver::driver_interface* drv);
		void draw_sky(rnd::driver::driver_interface* drv);
		void draw_ecs_model(rnd::driver::driver_interface* drv, rnd::shader_scene_desc& scene);
		void draw_ecs_meshes(ecs::entity ent, const rnd::driver::vertex_array_interface* va, rnd::shader_scene_desc& scene, rnd::driver::driver_interface* drv);
		void draw_ecs_meshes_transparant(ecs::entity ent, const rnd::driver::vertex_array_interface* va, rnd::shader_scene_desc& scene, rnd::driver::driver_interface* drv);
		void apply_material(ecs::entity material, rnd::shader_scene_desc& scene);
		void setup_instance_buffer();
		void z_prepass(rnd::driver::driver_interface* drv);
		void draw_transparent(rnd::driver::driver_interface* drv);
		void draw_composition(rnd::driver::driver_interface* drv, rnd::driver::texture_interface* color, rnd::driver::texture_interface* weight);
		void prepare_directional_light();

	public:
		int directional_light_count = 0;
		int point_light_count = 0;
		bool is_flag_test_render = true;
		bool is_flag_show_anim = true;
		std::vector<ecs::entity> scene_objects;
		std::unique_ptr<rnd::driver::vertex_array_interface> vertex_array;
		std::shared_ptr<rnd::driver::buffer_interface> vertex_buffer;
		std::shared_ptr<rnd::driver::buffer_interface> index_buffer;

		std::unique_ptr<rnd::driver::vertex_array_interface> vertex_array_inst;
		std::shared_ptr<rnd::driver::buffer_interface> matrices_buffer_inst;
		std::shared_ptr<rnd::driver::buffer_interface> vertex_buffer_inst;
		std::shared_ptr<rnd::driver::buffer_interface> index_buffer_inst;
	};

}