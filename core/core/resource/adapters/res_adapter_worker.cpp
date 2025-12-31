#include "res_adapter_worker.h"
#include "logger/engine_log.h"

core::res::async_adapter_worker::async_adapter_worker()
{
	work_guard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(io));

}

core::res::async_adapter_worker::~async_adapter_worker()
{
	stop();
}

void core::res::async_adapter_worker::start()
{	
	io_thread = std::thread([this]() {
		egLOG("resource/adapter", "io adapters thread started");

		while (true) {
			try {
				io.run();
				break;
			}
			catch (const std::exception& e) {
				egLOG("resource/adapter", "Exception in IO thread: {}", e.what());
				io.restart();
			}
		}

		egLOG("resource/adapter", "io adapter thread stopped");
	});
}

void core::res::async_adapter_worker::stop()
{
	work_guard.reset();
	io.stop();
	if (io_thread.joinable()) {
		io_thread.join();
	}
}

bool core::res::async_adapter_worker::is_stopped() const
{
	return io.stopped();
}

void core::res::async_adapter_worker::post(std::function<void()> cb)
{
	boost::asio::post(io.get_executor(), cb);
}