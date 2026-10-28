#pragma once
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/snk_api.h>

namespace core::res {
	// With a prj::project_context in `data`, mounts the project (throws prj::project_error when it is not safe to).
	SNK_API void resource_init(ds::app_data_storage& data);
	SNK_API void resource_term(ds::app_data_storage& data);
}