#include "gs_game_init.h"
#include "gs_game_system.h"

extern gs::game_system* p_game_system;

void components::game_init(ds::app_data_storage& data)
{
	auto& desc_sys = data.require<desc::desc_system>();
	p_game_system = &data.construct<gs::game_system>(desc_sys);
}

void components::game_term(ds::app_data_storage& data)
{
	data.destruct<gs::game_system>();
	p_game_system = nullptr;
}
