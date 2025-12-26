#pragma once
#include <ds/ds_store.hpp>

namespace components {

	void editor_init(ds::app_data_storage& data);
	void editor_term(ds::app_data_storage& data);
}

namespace com = components;