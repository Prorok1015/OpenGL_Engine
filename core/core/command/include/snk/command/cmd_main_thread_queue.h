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

		// `confirmation` is the ticket of a confirmation_required answer (call::confirmation) once a person approved it;
		// it is a parameter of its own and never read from the JSON arguments.
		// Safe from any thread. The future becomes ready after the drain that runs the call; the outcome is
		// whatever registry.invoke gives, so a command that is gone by then yields not_found. A call that never
		// runs (the queue is destroyed first, or the task system shuts down) yields "failed", and so does one whose
		// command throws (the message is the exception text). Do not wait on the future from the main thread
		// before the drain: nothing would run the call and the wait never ends. Code that can run on either
		// thread uses call_now_or_post() instead.
		[[nodiscard]] std::future<result> post(std::string id, boost::json::value args, origin from, std::uint64_t expected_generation = 0,
			std::string confirmation = {});

		// For a caller that may be on the main thread: there the command runs at once, inside this call, and the
		// future is already ready, so waiting on it cannot deadlock. This skips the queue, so it runs ahead of
		// calls that were posted earlier and are still waiting for the drain. On any other thread it is post().
		// Chosen over a future that asserts when waited on from the main thread: std::future cannot be hooked,
		// and the inline call keeps the usual "wait for the answer" code correct on both threads.
		[[nodiscard]] std::future<result> call_now_or_post(std::string id, boost::json::value args, origin from, std::uint64_t expected_generation = 0,
			std::string confirmation = {});

	private:
		struct state;

		ds::task_system& m_tasks;
		std::shared_ptr<state> m_state;
	};
}
