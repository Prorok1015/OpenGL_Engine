#include "gs_game_init.h"
#include "gs_game_system.h"

extern gs::GameSystem* p_game_system;

void components::game_init(ds::AppDataStorage& data)
{
	auto& desc_sys = data.require<desc::desc_system>();
	p_game_system = &data.construct<gs::GameSystem>(desc_sys);
}

void components::game_term(ds::AppDataStorage& data)
{
	data.destruct<gs::GameSystem>();
	p_game_system = nullptr;
}
