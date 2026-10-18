#pragma once
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/snk_api.h>

namespace scn
{
	SNK_API void scene_init(ds::app_data_storage& store);
	SNK_API void scene_term(ds::app_data_storage& store);
}