#pragma once
#include <ds_store.hpp>

namespace components {

	void gui_init(ds::app_data_storage& data);
	void gui_term(ds::app_data_storage& data);
}

namespace com = components;