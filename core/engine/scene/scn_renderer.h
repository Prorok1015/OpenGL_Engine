#pragma once
#include "rnd_renderer_base.h"
#include "scn_model.h"
#include "scn_camera_component.hpp"
#include "ecs_entity.h"
#include "shader/rnd_scene_shader_desc.h"
#include "scn_skinning_manager.h"

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
		renderer_3d(scn::skinning_manager& skin_mng);
		virtual ~renderer_3d() override;

		virtual void on_render(rnd::driver::driver_interface* drv);

		void draw_sky(rnd::driver::driver_interface* drv);
		void z_prepass(rnd::driver::driver_interface* drv);
		void draw_composition(rnd::driver::driver_interface* drv, rnd::driver::texture_interface* color, rnd::driver::texture_interface* weight);
		void prepare_directional_light();
		void draw_scene_by_material_desc(rnd::driver::driver_interface* drv, scn::pass_queue current_q);
		bool load_skin(rnd::driver::driver_interface* drv, entt::entity ent, entt::registry& registy);

	public:
		scn::skinning_manager& skin_manager;
		int directional_light_count = 0;
		int point_light_count = 0;
		bool is_flag_test_render = true;
		bool is_flag_show_anim = true;
		std::unique_ptr<rnd::driver::vertex_array_interface> vertex_array;
		std::shared_ptr<rnd::driver::buffer_interface> vertex_buffer;
		std::shared_ptr<rnd::driver::buffer_interface> index_buffer;
	};

}