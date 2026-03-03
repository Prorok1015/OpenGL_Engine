#include "gs_game_init.h"
#include "gs_game_system.h"
#include "scn_material_desc.h"
#include "scn_skinning_prototype_desc.h"
#include "level/scn_prefab_desc.h"
#include "scn_ecs_assembler.h"
#include "scn_anchor_desc.h"
#include "scn_camera_desc.h"
#include "scn_directional_light_desc.h"
#include "scn_skybox_desc.h"

extern gs::game_system* p_game_system;

void engine::game::game_init(ds::app_data_storage& data)
{
	auto& desc_sys = data.require<desc::desc_system>();
	p_game_system = &data.construct<gs::game_system>(desc_sys);

	desc_sys.register_desc<scn::material_desc>("material_desc"); // TODO: move to scene init
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

	auto& assembler = data.require<scn::ecs_assembler>();
	assembler.register_desc_spawner("prefab_desc", [&assembler] (entt::registry& reg, entt::entity e, const desc::desc_base& base_data, const std::string_view name) {
        const auto& prefab = static_cast<const scn::prefab_desc&>(base_data);
		scn::assemble_prefab(assembler, reg, e, prefab, name);
	});
	assembler.register_desc_spawner(scn::world_desc::__type, [&assembler] (entt::registry& reg, entt::entity e, const desc::desc_base& base_data, const std::string_view name) {
        const auto& prefab = static_cast<const scn::prefab_desc&>(base_data);
		scn::assemble_prefab(assembler, reg, e, prefab, name);
	});
	assembler.register_desc_spawner("prototype_desc", [] (entt::registry& reg, entt::entity e, const desc::desc_base& base_data, const std::string_view name) {
		const auto& proto = static_cast<const scn::prototype_desc&>(base_data);
		proto.load_prototype(reg, e);
	});
	assembler.register_desc_spawner("anim_prototype_desc", [] (entt::registry& reg, entt::entity e, const desc::desc_base& base_data, const std::string_view name) {
		const auto& proto = static_cast<const scn::prototype_desc&>(base_data);
		proto.load_prototype(reg, e);
	});
	assembler.register_desc_spawner("skin_prototype_desc", [] (entt::registry& reg, entt::entity e, const desc::desc_base& base_data, const std::string_view name) {
		const auto& proto = static_cast<const scn::prototype_desc&>(base_data);
		proto.load_prototype(reg, e);
	});
	assembler.register_desc_spawner("anchor_desc", [] (entt::registry& reg, entt::entity e, const desc::desc_base& base_data, const std::string_view name) {
		const auto& desc = static_cast<const scn::scene_anchor_desc&>(base_data);
		scn::assemble_scene_anchor(reg, e, desc, name);
	});
	assembler.register_desc_spawner("camera_desc", [] (entt::registry& reg, entt::entity e, const desc::desc_base& base_data, const std::string_view name) {
		const auto& desc = static_cast<const scn::camera_desc&>(base_data);
		scn::assemble_camera(reg, e, desc, name);
	});
	assembler.register_desc_spawner("directional_light_desc", [] (entt::registry& reg, entt::entity e, const desc::desc_base& base_data, const std::string_view name) {
		const auto& desc = static_cast<const scn::directional_light_desc&>(base_data);
		scn::assemble_directional_light(reg, e, desc, name);
	});
	assembler.register_desc_spawner("skybox_desc", [] (entt::registry& reg, entt::entity e, const desc::desc_base& base_data, const std::string_view name) {
		const auto& desc = static_cast<const scn::skybox_desc&>(base_data);
		scn::assemble_skybox(reg, e, desc, name);
	});
}

void engine::game::game_term(ds::app_data_storage& data)
{
	auto& assembler = data.require<scn::ecs_assembler>();
	assembler.unregister_desc_spawner("prefab_desc");
	assembler.unregister_desc_spawner("prototype_desc");
	assembler.unregister_desc_spawner("anim_prototype_desc");
	assembler.unregister_desc_spawner("skin_prototype_desc");

	auto& desc_sys = data.require<desc::desc_system>();
	desc_sys.unregister_desc("material_desc");
	desc_sys.unregister_desc("prototype_desc");
	desc_sys.unregister_desc("anim_prototype_desc");
	desc_sys.unregister_desc("skin_prototype_desc");
	data.destruct<gs::game_system>();
	p_game_system = nullptr;
}
