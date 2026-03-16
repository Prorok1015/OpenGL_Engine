#pragma once
#include "ds/ds_store.hpp"

namespace engine::game {
	void game_init(ds::app_data_storage& data);
	void game_term(ds::app_data_storage& data);
}