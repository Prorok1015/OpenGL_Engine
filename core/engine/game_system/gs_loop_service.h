#pragma once
#include <chrono>
#include "app_loop_service_interface.h"

namespace gs
{
	class gs_loop_service : public app::app_loop_service_interface
	{
	public:
		gs_loop_service();
		virtual ~gs_loop_service();
		virtual void init(ds::app_data_storage& storage) override;
		virtual void on_step(ds::app_data_storage& storage) override;
		virtual bool should_stop() const override;
	private:
		bool stop_requested = false;
		float delta_time = 1.0f / 60.0f;
		std::chrono::steady_clock::time_point previous_time;
	};
}