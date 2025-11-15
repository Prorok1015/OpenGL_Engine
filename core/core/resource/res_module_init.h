#pragma once
#include <ds_store.hpp>

namespace components {

	void resource_init(ds::app_data_storage& data);
	void resource_term(ds::app_data_storage& data);
}

namespace com = components;