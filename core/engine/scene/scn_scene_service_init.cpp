#include "scn_scene_service_init.h"
#include "ecs_system.h"
#include "scn_animation_job.h"
#include "scn_transform_system.h"
#include "scn_camera_controller_system.h"

void scn::scene_init(ds::app_data_storage& store)
{
	auto& sfactory = store.require<ecs::system_factory>();
	sfactory.register_system("scn::animation_system", init_animation_system);
	sfactory.register_system("scn::transform_system", init_transform_system);
	sfactory.register_system("scn::mouse_controller_system", init_mouse_controller_system);
}

void scn::scene_term(ds::app_data_storage& store)
{
	auto& sfactory = store.require<ecs::system_factory>();
	sfactory.unregister_system("scn::animation_system");
	sfactory.unregister_system("scn::transform_system");
	sfactory.unregister_system("scn::mouse_controller_system");
}