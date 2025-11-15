#pragma once
#include "ds_store.hpp"

namespace app
{
	struct app_loop_service_interface
	{
		virtual ~app_loop_service_interface() = default;
		virtual void init(ds::app_data_storage&) = 0;
		virtual void on_step(ds::app_data_storage&) = 0;
		virtual bool should_stop() const = 0;
	};
}