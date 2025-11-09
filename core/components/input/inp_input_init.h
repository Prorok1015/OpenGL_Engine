#pragma once
#include <ds_store.hpp>

namespace components {

	void input_init(ds::app_data_storage& data);
	void input_term(ds::app_data_storage& data);
}

namespace com = components;