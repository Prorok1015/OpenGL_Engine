#pragma once
#include "ds/ds_store.hpp"

namespace core::resource {
	void resource_init(ds::app_data_storage& data);
	void resource_term(ds::app_data_storage& data);
}