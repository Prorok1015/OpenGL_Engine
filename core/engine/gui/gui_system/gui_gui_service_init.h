#pragma once
#include "ds/ds_store.hpp"

namespace engine::gui {
	void gui_init(ds::app_data_storage& data);
	void gui_term(ds::app_data_storage& data);
}