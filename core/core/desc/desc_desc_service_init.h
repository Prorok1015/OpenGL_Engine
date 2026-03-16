#pragma once
#include "ds/ds_store.hpp"

namespace core::desc {
	void desc_init(ds::app_data_storage& data);
	void desc_term(ds::app_data_storage& data);
}