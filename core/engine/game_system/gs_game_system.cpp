#include "gs_game_system.h"

#include <inp_input_manager.h>
#include <inp_input_system.h>

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
#include "scn_skinning_manager.h"
#include "ecs_component.h"

#include "geom/rnd_geometry_desc.h"
#include "texture/rnd_texture_desc.h"

gs::game_system* p_game_system = nullptr;
extern int gMaxTexture2DSize;

scn::mouse_controller_job mouse_controller_system;
scn::transform_job transform_job_instance;
scn::animation_job animation_job_instance;

gs::game_system& gs::get_system()
{
	ASSERT_MSG(p_game_system, "Game system is nullptr!");
	return *p_game_system;
}

std::unique_ptr<scn::skinning_manager> skin_manager;

gs::game_system::game_system(desc::desc_system& d)
	: desc_system(d)
{

	skin_manager = std::make_unique<scn::skinning_manager>(d);

	renderer = std::make_shared<scn::renderer_3d>(*skin_manager);
	rnd::get_system().activate_renderer(renderer);
}

gs::game_system::~game_system()
{
	skin_manager.reset();
	rnd::get_system().deactivate_renderer(renderer);
}

void gs::game_system::set_enable_input(bool enable)
{
}


// TODO: remove
void gs::game_system::end_ecs_frame()
{
	const auto ents = ecs::registry.view<ecs::input_changed_event_component>();
	ecs::registry.destroy(ents.begin(), ents.end());

	inp::get_system().mouse.clear_scroll();
}

void gs::game_system::reload_shaders()
{
	rnd::get_system().get_shader_manager().clear_cache();
}


