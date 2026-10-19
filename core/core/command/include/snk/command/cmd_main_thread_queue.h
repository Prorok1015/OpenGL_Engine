#pragma once
#include "cmd_registry.h"
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/common/snk_api.h>

namespace cmd
{
	// Lets any thread run a command: the call waits in the main queue of ds::task_system and the registry
	// runs it when the `tasks` frame stage drains that queue (task_system::drain_main), on the main thread.
	// There is no second drain.
	// Create and destroy it on the main thread; destroy it before the registry and the task system.
	class SNK_API main_thread_queue
	{
	public:
		main_thread_queue(command_registry& registry, ds::task_system& tasks);
		~main_thread_queue();
		main_thread_queue(const main_thread_queue&) = delete;
		main_thread_queue& operator=(const main_thread_queue&) = delete;

		// Safe from any thread. The future becomes ready after the drain that runs the call; the outcome is
		// whatever registry.invoke gives, so a command that is gone by then yields not_found. A call that never
		// runs (the queue is destroyed first, or the task system shuts down) yields "failed". Do not wait on the
		// future from the main thread before the drain: nothing would run the call.
		[[nodiscard]] std::future<result> post(std::string id, boost::json::value args, origin from, std::uint64_t expected_generation = 0);

	private:
		struct state;

		ds::task_system& m_tasks;
		std::shared_ptr<state> m_state;
	};
}
