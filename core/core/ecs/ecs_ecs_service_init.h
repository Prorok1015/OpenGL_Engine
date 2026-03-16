#pragma once
#include "ds/ds_store.hpp"

namespace ecs
{
	void ecs_init(ds::app_data_storage& data);
	void ecs_term(ds::app_data_storage& data);
}