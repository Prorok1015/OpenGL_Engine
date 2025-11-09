#pragma once
#include <ds_store.hpp>

namespace components {

	void game_init(ds::app_data_storage& data);
	void game_term(ds::app_data_storage& data);
}

namespace com = components;