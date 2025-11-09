#pragma once
#include <ds_store.hpp>

namespace components{

	void component_init(ds::app_data_storage& data);
	void component_term(ds::app_data_storage& data);
}

namespace com = components;