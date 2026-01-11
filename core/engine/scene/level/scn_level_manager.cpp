#include "scn_level_manager.h"


void scn::level_manager::update(std::chrono::duration<float> dt)
{
	active_lvl.update(dt);
}