#include "gs_game_system.h"

#include <scn_model.h>

#include <rnd_render_system.h>
#include <ecs_common_system.h>

#include "scn_transform_system.h"
#include "scn_animation_job.h"
#include "scn_skinning_manager.h"
#include "ecs_component.h"

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

}

gs::game_system::~game_system()
{
	skin_manager.reset();
}

void gs::game_system::set_enable_input(bool enable)
{
}


// TODO: remove
void gs::game_system::end_ecs_frame()
{
	const auto ents = ecs::registry.view<ecs::input_changed_event_component>();
	ecs::registry.destroy(ents.begin(), ents.end());
}

void gs::game_system::reload_shaders()
{
	rnd::get_system().get_shader_manager().clear_cache();
}


