#pragma once
// Aborts the test run when a scope outlives its limit: a regression that hangs (a deadlock between a job and the main
// thread) fails CI with a log line instead of stalling it until the runner's timeout.
#include <snk/common/logger/engine_log.h>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace test_util {

class watchdog
{
public:
	watchdog(std::string what, std::chrono::seconds limit)
		: m_thread([this, what = std::move(what), limit] {
			std::unique_lock lock(m_mutex);
			if (!m_finished.wait_for(lock, limit, [this] { return m_done; })) {
				egLOG_ERROR("test", "still running after {} s, aborting: {}", limit.count(), what);
				std::abort();
			}
		})
	{
	}

	~watchdog()
	{
		{
			std::lock_guard lock(m_mutex);
			m_done = true;
		}
		m_finished.notify_all();
		m_thread.join();
	}

	watchdog(const watchdog&) = delete;
	watchdog& operator=(const watchdog&) = delete;

private:
	std::mutex m_mutex;
	std::condition_variable m_finished;
	bool m_done = false;
	// Last: started once the members it reads exist.
	std::thread m_thread;
};

}
