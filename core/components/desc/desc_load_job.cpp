#include "desc_load_job.h"
#include "entt/entt.hpp"

void desc::desc_load_job::init(entt::organizer& organizer, entt::registry& registry)
{
	organizer.emplace<&desc::desc_load_job::update>(*this, "desc_load_job_update");
}

void desc::desc_load_job::deinit(entt::organizer& organizer, entt::registry& registry)
{

}

void desc::desc_load_job::update()
{
	if (desc_system) {
		desc_system->finish_descs();
	}
}
