#pragma once
#include <ds_store.hpp>

namespace components {

	void render_init(ds::app_data_storage& data);
	void render_term(ds::app_data_storage& data);
}

namespace com = components;