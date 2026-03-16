#pragma once
#include "ds/ds_store.hpp"

namespace scn
{
	void scene_init(ds::app_data_storage& store);
	void scene_term(ds::app_data_storage& store);
}