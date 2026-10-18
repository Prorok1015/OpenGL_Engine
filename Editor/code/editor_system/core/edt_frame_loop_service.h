#pragma once
#include "app_loop_service_interface.h"
#include "app_frame_stage.h"
#include <chrono>

namespace edt {
	class edt_loop_service : public app::app_loop_service_interface {
	public:
		edt_loop_service();
		virtual ~edt_loop_service();
		virtual void init(ds::app_data_storage& storage) override;
		virtual void on_step(ds::app_data_storage& storage) override;
		virtual bool should_stop() const override;
		virtual void request_stop() override;

	private:
		bool stop_requested = false;
		float delta_time = 1.0f / 60.0f;
		std::chrono::steady_clock::time_point previous_time;
		app::frame_stage_registry m_stages;
	};
}