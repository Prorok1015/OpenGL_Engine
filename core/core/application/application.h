#pragma once
#include <common.h>
#include "ds_store.hpp"

namespace app
{
	class application
	{

	public:
		application();
		virtual ~application();
		application(application&&) = delete;
		application& operator= (application&&) = delete;
		application(const application&) = delete;
		application& operator= (const application&) = delete;

		virtual int run(ds::app_data_storage& storage);
	private:
	};

	application& get_app_system();
}
