#include "scn_scene_service_init.h"
#include "ecs_system.h"
#include "scn_animation_job.h"
#include "scn_transform_system.h"
#include "scn_camera_controller_system.h"
#include "level/scn_level_manager.h"
#include "scn_ecs_assembler.h"
#include "adapters/scn_worldwrap_adapter.h"
#include "res_system.h"

void scn::scene_init(ds::app_data_storage& store)
{
	auto& desc_sys = store.require<desc::desc_system>();
	auto& sfactory = store.require<ecs::system_factory>();
	auto& resource = store.require<res::resource_system>();

	auto& assembler = store.construct<scn::ecs_assembler>(desc_sys);
	init_animation_system(sfactory);
	init_transform_system(sfactory);
	init_mouse_controller_system(sfactory);
	store.construct<scn::level_manager>(resource, assembler, sfactory);
	resource.registrate_adapter<scn::worldwrap_adapter>(scn::worldwrap_adapter::INFO, resource, desc_sys);
}

void scn::scene_term(ds::app_data_storage& store)
{
	auto& resource = store.require<res::resource_system>();
	resource.unregistrate_adapter(scn::worldwrap_adapter::INFO);
	store.destruct<scn::level_manager>();
	auto& sfactory = store.require<ecs::system_factory>();
	store.destruct<scn::ecs_assembler>();
}