#include "ecs_ecs_service_init.h"
#include "ecs_system.h"

void ecs::ecs_init(ds::app_data_storage& data)
{
	data.construct<ecs::system_factory>();
}

void ecs::ecs_term(ds::app_data_storage& data)
{
	data.destruct<ecs::system_factory>();
}
