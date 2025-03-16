#pragma once
#include "ds_store.hpp"

namespace components {

	void desc_init(ds::AppDataStorage& data);
	void desc_term(ds::AppDataStorage& data);
}

namespace com = components;