#include "scn_scene_service_init.h"
#include "ecs_system.h"
#include "scn_animation_job.h"
#include "scn_transform_system.h"
#include "scn_camera_controller_system.h"
#include "level/scn_level_manager.h"

void scn::scene_init(ds::app_data_storage& store)
{
	store.construct<scn::level_manager>();
	auto& sfactory = store.require<ecs::system_factory>();
	init_animation_system(sfactory);
	init_transform_system(sfactory);
	init_mouse_controller_system(sfactory);
}

void scn::scene_term(ds::app_data_storage& store)
{
	auto& sfactory = store.require<ecs::system_factory>();
	store.destruct<scn::level_manager>();
}