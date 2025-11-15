#pragma once
#include <ds_store.hpp>

namespace components {

	void window_init(ds::app_data_storage& data);
	void window_term(ds::app_data_storage& data);
}

namespace com = components;