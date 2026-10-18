#pragma once
#include "ds/ds_store.hpp"

namespace app
{
	struct app_loop_service_interface
	{
		virtual ~app_loop_service_interface() = default;
		virtual void init(ds::app_data_storage&) = 0;
		virtual void on_step(ds::app_data_storage&) = 0;
		// Counterpart of init(): called once after the last step, also when a step threw.
		virtual void term(ds::app_data_storage&) {}
		virtual bool should_stop() const = 0;
		// Asks the loop to finish after the current step (window closed, game over, ...).
		virtual void request_stop() = 0;
	};
}