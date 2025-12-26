#pragma once
#include "ds/ds_store.hpp"

namespace engine::input {
	void input_init(ds::app_data_storage& data);
	void input_term(ds::app_data_storage& data);
}