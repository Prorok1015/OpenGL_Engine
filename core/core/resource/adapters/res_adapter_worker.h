#pragma once
#include "res_adapter_worker_base.hpp"
#include <boost/asio.hpp>
#include <functional>

namespace core::res
{
	class async_adapter_worker : public adapter_worker_base
	{
	public:
		async_adapter_worker();
		virtual ~async_adapter_worker() override;

		virtual void start() override;
		virtual void stop() override;
		virtual bool is_stopped() const override;
		virtual void post(std::function<void()> cb) override;
	private:
		boost::asio::io_context io;
		std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard;
		std::thread io_thread;
	};
}