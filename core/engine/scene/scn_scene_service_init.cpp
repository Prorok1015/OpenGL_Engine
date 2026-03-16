#include "scn_scene_service_init.h"
#include "adapters/scn_worldwrap_adapter.h"
#include "ecs_system.h"
#include "level/scn_hot_reload_manager.h"
#include "level/scn_level_manager.h"
#include "res_system.h"
#include "scn_animation_job.h"
#include "scn_camera_controller_system.h"
#include "scn_ecs_assembler.h"
#include "scn_transform_system.h"


#include "level/scn_light_extractor.h"
#include "level/scn_render_data_extractor.h"
#include "rnd_frame_assembler.h"
#include "skinning/rnd_skinning_manager.h"
#include "rnd_render_system.h"

void scn::scene_init(ds::app_data_storage &store) {
  auto &desc_sys = store.require<desc::desc_system>();
  auto &sfactory = store.require<ecs::system_factory>();
  auto &resource = store.require<res::resource_system>();

  auto &assembler = store.construct<scn::ecs_assembler>(desc_sys);
  init_animation_system(sfactory);
  init_transform_system(sfactory);
  init_mouse_controller_system(sfactory);
  store.construct<scn::hot_reload_manager>(store.require<scn::ecs_assembler>(),
                                           resource);

  store.construct<scn::level_manager>(
      resource, assembler, sfactory,
      store.require_shared<scn::hot_reload_manager>());
  store.construct<rnd::skinning_manager>();
  resource.registrate_adapter<scn::worldwrap_adapter>(
      scn::worldwrap_adapter::INFO, resource, desc_sys);

  auto &frame_assembler = store.construct<rnd::frame_assembler>();
  frame_assembler.add_extractor(std::make_shared<scn::light_extractor>(
      store.require<scn::level_manager>()));
  frame_assembler.add_extractor(std::make_shared<scn::render_data_extractor>(
      store.require<scn::level_manager>()));

  auto& render_system = store.require<rnd::render_system>();
  render_system.set_skinning_manager(&store.require<rnd::skinning_manager>());
}

void scn::scene_term(ds::app_data_storage &store) {
	store.destruct<rnd::frame_assembler>();
  store.destruct<scn::hot_reload_manager>();
  store.destruct<rnd::skinning_manager>();
  auto &resource = store.require<res::resource_system>();
  resource.unregistrate_adapter(scn::worldwrap_adapter::INFO);
  store.destruct<scn::level_manager>();
  auto &sfactory = store.require<ecs::system_factory>();
  store.destruct<scn::ecs_assembler>();
}