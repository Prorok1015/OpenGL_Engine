#include "scn_level.h"

void scn::level::update(std::chrono::duration<float> dt)
{
	for (auto& [_, world] : m_worlds)
		world->update(dt);
}