#pragma once
#include <snk/common/common.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/snk_api.h>

namespace app
{
	class SNK_API application
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
}
