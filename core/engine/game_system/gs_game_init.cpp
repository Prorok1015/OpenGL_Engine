#include "gs_game_init.h"
#include "gs_game_system.h"
#include "scn_material_desc.h"
#include "scn_skinning_prototype_desc.h"

extern gs::game_system* p_game_system;

void engine::game::game_init(ds::app_data_storage& data)
{
	auto& desc_sys = data.require<desc::desc_system>();
	p_game_system = &data.construct<gs::game_system>(desc_sys);
	desc_sys.register_desc<scn::material_desc>("material_desc"); // TODO: move to scene init
	desc_sys.register_desc<scn::prototype_desc>("prototype_desc");
	desc_sys.register_desc<scn::animatable_prototype_desc>("anim_prototype_desc");
	desc_sys.register_desc<scn::skinning_prototype_desc>("skin_prototype_desc");
}

void engine::game::game_term(ds::app_data_storage& data)
{
	auto& desc_sys = data.require<desc::desc_system>();
	desc_sys.unregister_desc("material_desc");
	desc_sys.unregister_desc("prototype_desc");
	desc_sys.unregister_desc("anim_prototype_desc");
	desc_sys.unregister_desc("skin_prototype_desc");
	data.destruct<gs::game_system>();
	p_game_system = nullptr;
}
