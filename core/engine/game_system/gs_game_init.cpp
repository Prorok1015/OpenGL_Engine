#include "gs_game_init.h"
#include "gs_game_system.h"
#include "level/scn_level_desc.h"
#include "level/scn_prefab_desc.h"
#include "level/scn_world_desc.h"
#include "scn_anchor_desc.h"
#include "scn_bone_desc.h"
#include "scn_camera_desc.h"
#include "scn_directional_light_desc.h"
#include "scn_ecs_assembler.h"
#include "scn_material_desc.h"
#include "scn_mesh_node_desc.h"
#include "scn_object_desc.h"
#include "scn_skin_weights_desc.h"
#include "scn_skinning_desc.h"
#include "scn_skybox_desc.h"
#include "scn_animation_controller_desc.h"
#include "scn_animated_node_desc.h"
#include "scn_animation_clip_desc.h"
#include "scn_animation_collection_desc.h"

extern gs::game_system *p_game_system;

void engine::game::game_init(ds::app_data_storage &data) {
  auto &desc_sys = data.require<desc::desc_system>();
  p_game_system = &data.construct<gs::game_system>(desc_sys);

  desc_sys.register_desc<scn::material_desc>(
      "material_desc"); // TODO: move all register into scene init
  desc_sys.register_desc<scn::prefab_desc>("prefab_desc");
  desc_sys.register_desc<scn::world_desc>(scn::world_desc::__type);
  desc_sys.register_desc<scn::level_desc>("level_desc");
  desc_sys.register_desc<scn::scene_anchor_desc>("anchor_desc");
  desc_sys.register_component_desc<scn::camera_desc>(scn::camera_desc::__type);
  desc_sys.register_component_desc<scn::directional_light_desc>(scn::directional_light_desc::__type);
  desc_sys.register_component_desc<scn::skybox_desc>(scn::skybox_desc::__type);
  desc_sys.register_desc<scn::mesh_node_desc>(scn::mesh_node_desc::__type);
  desc_sys.register_desc<scn::object_desc>(scn::object_desc::__type);
  // animations_desc and keyframes_desc removed — replaced by animation_clip_desc + animation_collection_desc
  desc_sys.register_desc<scn::bone_desc>(scn::bone_desc::__type);
  desc_sys.register_desc<scn::skinning_desc>(scn::skinning_desc::__type);
  desc_sys.register_desc<scn::skin_weights_desc>(scn::skin_weights_desc::__type);
  desc_sys.register_desc<scn::animation_controller_desc>(scn::animation_controller_desc::__type);
  desc_sys.register_desc<scn::animated_node_desc>(scn::animated_node_desc::__type);
  desc_sys.register_desc<scn::animation_clip_desc>(scn::animation_clip_desc::__type);
  desc_sys.register_desc<scn::animation_collection_desc>(scn::animation_collection_desc::__type);

  auto &assembler = data.require<scn::ecs_assembler>();
  assembler.register_desc_spawner(
      "prefab_desc",
      [&assembler](entt::registry &reg, entt::entity e,
                   const desc::desc_base &base_data,
                   const std::string_view name) {
        const auto &prefab = static_cast<const scn::prefab_desc &>(base_data);
        scn::assemble_prefab(assembler, reg, e, prefab, name, prefab.get_tag());
      },
      nullptr, scn::spawner_category::structural);

  assembler.register_desc_spawner(
      scn::world_desc::__type,
      [&assembler](entt::registry &reg, entt::entity e,
                   const desc::desc_base &base_data,
                   const std::string_view name) {
        const auto &prefab = static_cast<const scn::prefab_desc &>(base_data);
        scn::assemble_prefab(assembler, reg, e, prefab, name, prefab.get_tag());
      },
      nullptr, scn::spawner_category::structural);

  assembler.register_desc_spawner(
      "anchor_desc",
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc =
            static_cast<const scn::scene_anchor_desc &>(base_data);
        scn::assemble_scene_anchor(reg, e, desc, name);
      });

  assembler.register_desc_spawner(
      "camera_desc",
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc = static_cast<const scn::camera_desc &>(base_data);
        scn::assemble_camera(reg, e, desc, name);
      });

  assembler.register_desc_spawner(
      "directional_light_desc",
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc =
            static_cast<const scn::directional_light_desc &>(base_data);
        scn::assemble_directional_light(reg, e, desc, name);
      });

  assembler.register_desc_spawner(
      "skybox_desc",
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc = static_cast<const scn::skybox_desc &>(base_data);
        scn::assemble_skybox(reg, e, desc, name);
      });

  assembler.register_desc_spawner(
      scn::mesh_node_desc::__type,
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc = static_cast<const scn::mesh_node_desc &>(base_data);
        scn::assemble_mesh_node(reg, e, desc, name);
      });

  assembler.register_desc_spawner(
      scn::object_desc::__type,
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc = static_cast<const scn::object_desc &>(base_data);
        scn::assemble_object(reg, e, desc, name);
      });

  // animations_desc and keyframes_desc spawners removed — replaced by animation_collection_desc

  assembler.register_desc_spawner(
      scn::bone_desc::__type,
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc = static_cast<const scn::bone_desc &>(base_data);
        scn::assemble_bone(reg, e, desc, name);
      });

  assembler.register_desc_spawner(
      scn::skinning_desc::__type,
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc = static_cast<const scn::skinning_desc &>(base_data);
        scn::assemble_skinning(reg, e, desc, name);
      });

  assembler.register_desc_spawner(
      scn::skin_weights_desc::__type,
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc = static_cast<const scn::skin_weights_desc &>(base_data);
        scn::assemble_skin_weights(reg, e, desc, name);
      });

  assembler.register_desc_spawner(
      scn::animation_controller_desc::__type,
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc = static_cast<const scn::animation_controller_desc &>(base_data);
        scn::assemble_animation_controller(reg, e, desc, name);
      });

  assembler.register_desc_spawner(
      scn::animated_node_desc::__type,
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc = static_cast<const scn::animated_node_desc &>(base_data);
        scn::assemble_animated_node(reg, e, desc, name);
      });

  assembler.register_desc_spawner(
      scn::animation_collection_desc::__type,
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc = static_cast<const scn::animation_collection_desc &>(base_data);
        scn::assemble_animation_collection(reg, e, desc, name);
      });
}

void engine::game::game_term(ds::app_data_storage &data) {
  auto &assembler = data.require<scn::ecs_assembler>();
  assembler.unregister_desc_spawner("skybox_desc");
  assembler.unregister_desc_spawner("directional_light_desc");
  assembler.unregister_desc_spawner("camera_desc");
  assembler.unregister_desc_spawner("anchor_desc");
  assembler.unregister_desc_spawner("prefab_desc");

  assembler.unregister_desc_spawner(scn::animation_collection_desc::__type);
  assembler.unregister_desc_spawner(scn::animated_node_desc::__type);
  assembler.unregister_desc_spawner(scn::animation_controller_desc::__type);
  assembler.unregister_desc_spawner(scn::skin_weights_desc::__type);
  assembler.unregister_desc_spawner(scn::skinning_desc::__type);
  assembler.unregister_desc_spawner(scn::bone_desc::__type);
  // keyframes_desc and animations_desc spawners removed
  assembler.unregister_desc_spawner(scn::mesh_node_desc::__type);
  assembler.unregister_desc_spawner(scn::object_desc::__type);

  auto &desc_sys = data.require<desc::desc_system>();
  desc_sys.unregister_desc("material_desc");

  desc_sys.unregister_desc(scn::animation_collection_desc::__type);
  desc_sys.unregister_desc(scn::animation_clip_desc::__type);
  desc_sys.unregister_desc(scn::animated_node_desc::__type);
  desc_sys.unregister_desc(scn::animation_controller_desc::__type);
  desc_sys.unregister_desc(scn::skin_weights_desc::__type);
  desc_sys.unregister_desc(scn::skinning_desc::__type);
  desc_sys.unregister_desc(scn::bone_desc::__type);
  // keyframes_desc and animations_desc unregister removed
  desc_sys.unregister_desc(scn::mesh_node_desc::__type);
  desc_sys.unregister_desc(scn::object_desc::__type);
  data.destruct<gs::game_system>();
  p_game_system = nullptr;
}
