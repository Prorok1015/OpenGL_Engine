#include "gs_game_init.h"
#include "gs_game_system.h"
#include "scn_material_desc.h"
#include "scn_skinning_prototype_desc.h"
#include "level/scn_prefab_desc.h"
#include "scn_ecs_assembler.h"

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

	auto& assembler = data.require<scn::ecs_assembler>();
	assembler.register_desc_spawner("prefab_desc", [&assembler] (entt::registry& reg, entt::entity e, const desc::desc_base& base_data, const std::string& name) {
        const auto& prefab = static_cast<const scn::prefab_desc&>(base_data);
		scn::assemble_prefab(assembler, reg, e, prefab, name);
	});
	assembler.register_desc_spawner("prototype_desc", [] (entt::registry& reg, entt::entity e, const desc::desc_base& base_data, const std::string& name) {
		const auto& proto = static_cast<const scn::prototype_desc&>(base_data);
		proto.load_prototype(reg, e);
	});
	assembler.register_desc_spawner("anim_prototype_desc", [] (entt::registry& reg, entt::entity e, const desc::desc_base& base_data, const std::string& name) {
		const auto& proto = static_cast<const scn::prototype_desc&>(base_data);
		proto.load_prototype(reg, e);
	});
	assembler.register_desc_spawner("skin_prototype_desc", [] (entt::registry& reg, entt::entity e, const desc::desc_base& base_data, const std::string& name) {
		const auto& proto = static_cast<const scn::prototype_desc&>(base_data);
		proto.load_prototype(reg, e);
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
