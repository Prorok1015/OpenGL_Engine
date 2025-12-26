#pragma once
#include "ds/ds_store.hpp"

namespace engine::render {
	void render_init(ds::app_data_storage& data);
	void render_term(ds::app_data_storage& data);
}