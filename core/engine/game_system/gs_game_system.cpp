#include "gs_game_system.h"

#include <scn_model.h>

#include <rnd_render_system.h>
#include <ecs_common_system.h>

#include "scn_transform_system.h"
#include "scn_animation_job.h"
#include "skinning/rnd_skinning_manager.h"
#include "ecs_component.h"

gs::game_system* p_game_system = nullptr;
extern int gMaxTexture2DSize;

gs::game_system& gs::get_system()
{
	ASSERT_MSG(p_game_system, "Game system is nullptr!");
	return *p_game_system;
}

std::unique_ptr<rnd::skinning_manager> skin_manager;

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

void gs::game_system::reload_shaders()
{
	rnd::get_system().get_shader_manager().clear_cache();
}


