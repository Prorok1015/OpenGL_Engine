#pragma once
#include "ds_store.hpp"

namespace components {

	void desc_init(ds::app_data_storage& data);
	void desc_term(ds::app_data_storage& data);
}

namespace com = components;