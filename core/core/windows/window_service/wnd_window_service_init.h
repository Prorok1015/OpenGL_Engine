#pragma once
#include "ds/ds_store.hpp"

namespace core::window {
	void window_init(ds::app_data_storage& data);
	void window_term(ds::app_data_storage& data);
}