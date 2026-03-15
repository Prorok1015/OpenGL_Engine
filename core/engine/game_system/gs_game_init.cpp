#include "gs_game_init.h"
#include "gs_game_system.h"
#include "level/scn_level_desc.h"
#include "level/scn_prefab_desc.h"
#include "level/scn_world_desc.h"
#include "scn_anchor_desc.h"
#include "scn_animations_desc.h"
#include "scn_bone_desc.h"
#include "scn_camera_desc.h"
#include "scn_directional_light_desc.h"
#include "scn_ecs_assembler.h"
#include "scn_keyframes_desc.h"
#include "scn_material_desc.h"
#include "scn_mesh_node_desc.h"
#include "scn_object_desc.h"
#include "scn_skinning_desc.h"
#include "scn_skinning_prototype_desc.h"
#include "scn_skybox_desc.h"

extern gs::game_system *p_game_system;

void engine::game::game_init(ds::app_data_storage &data) {
  auto &desc_sys = data.require<desc::desc_system>();
  p_game_system = &data.construct<gs::game_system>(desc_sys);

  desc_sys.register_desc<scn::material_desc>(
      "material_desc"); // TODO: move all register into scene init
  desc_sys.register_desc<scn::prototype_desc>("prototype_desc");
  desc_sys.register_desc<scn::animatable_prototype_desc>("anim_prototype_desc");
  desc_sys.register_desc<scn::skinning_prototype_desc>("skin_prototype_desc");
  desc_sys.register_desc<scn::prefab_desc>("prefab_desc");
  desc_sys.register_desc<scn::world_desc>(scn::world_desc::__type);
  desc_sys.register_desc<scn::level_desc>("level_desc");
  desc_sys.register_desc<scn::scene_anchor_desc>("anchor_desc");
  desc_sys.register_desc<scn::camera_desc>("camera_desc");
  desc_sys.register_desc<scn::directional_light_desc>("directional_light_desc");
  desc_sys.register_desc<scn::skybox_desc>("skybox_desc");
  desc_sys.register_desc<scn::mesh_node_desc>(scn::mesh_node_desc::__type);
  desc_sys.register_desc<scn::object_desc>(scn::object_desc::__type);
  desc_sys.register_desc<scn::animations_desc>(scn::animations_desc::__type);
  desc_sys.register_desc<scn::keyframes_desc>(scn::keyframes_desc::__type);
  desc_sys.register_desc<scn::bone_desc>(scn::bone_desc::__type);
  desc_sys.register_desc<scn::skinning_desc>(scn::skinning_desc::__type);

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
      "prototype_desc",
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &proto = static_cast<const scn::prototype_desc &>(base_data);
        proto.load_prototype(reg, e);
      });

  assembler.register_desc_spawner(
      "anim_prototype_desc",
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &proto = static_cast<const scn::prototype_desc &>(base_data);
        proto.load_prototype(reg, e);
      });

  assembler.register_desc_spawner(
      "skin_prototype_desc",
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &proto = static_cast<const scn::prototype_desc &>(base_data);
        proto.load_prototype(reg, e);
      });

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

  assembler.register_desc_spawner(
      scn::animations_desc::__type,
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc = static_cast<const scn::animations_desc &>(base_data);
        scn::assemble_animations(reg, e, desc, name);
      });

  assembler.register_desc_spawner(
      scn::keyframes_desc::__type,
      [](entt::registry &reg, entt::entity e, const desc::desc_base &base_data,
         const std::string_view name) {
        const auto &desc = static_cast<const scn::keyframes_desc &>(base_data);
        scn::assemble_keyframes(reg, e, desc, name);
      });

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
}

void engine::game::game_term(ds::app_data_storage &data) {
  auto &assembler = data.require<scn::ecs_assembler>();
  assembler.unregister_desc_spawner("skybox_desc");
  assembler.unregister_desc_spawner("directional_light_desc");
  assembler.unregister_desc_spawner("camera_desc");
  assembler.unregister_desc_spawner("anchor_desc");
  assembler.unregister_desc_spawner("prefab_desc");
  assembler.unregister_desc_spawner("prototype_desc");
  assembler.unregister_desc_spawner("anim_prototype_desc");
  assembler.unregister_desc_spawner("skin_prototype_desc");
  assembler.unregister_desc_spawner(scn::skinning_desc::__type);
  assembler.unregister_desc_spawner(scn::bone_desc::__type);
  assembler.unregister_desc_spawner(scn::keyframes_desc::__type);
  assembler.unregister_desc_spawner(scn::animations_desc::__type);
  assembler.unregister_desc_spawner(scn::mesh_node_desc::__type);
  assembler.unregister_desc_spawner(scn::object_desc::__type);

  auto &desc_sys = data.require<desc::desc_system>();
  desc_sys.unregister_desc("material_desc");
  desc_sys.unregister_desc("prototype_desc");
  desc_sys.unregister_desc("anim_prototype_desc");
  desc_sys.unregister_desc("skin_prototype_desc");
  desc_sys.unregister_desc(scn::skinning_desc::__type);
  desc_sys.unregister_desc(scn::bone_desc::__type);
  desc_sys.unregister_desc(scn::keyframes_desc::__type);
  desc_sys.unregister_desc(scn::animations_desc::__type);
  desc_sys.unregister_desc(scn::mesh_node_desc::__type);
  desc_sys.unregister_desc(scn::object_desc::__type);
  data.destruct<gs::game_system>();
  p_game_system = nullptr;
}
