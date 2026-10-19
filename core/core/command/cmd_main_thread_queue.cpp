#include <snk/command/cmd_main_thread_queue.h>
#include <snk/common/engine_assert.h>
#include <utility>

namespace {
	// Settles the promise with `failed` if the posted call is dropped without running (the task system
	// discards its queue on shutdown), so a waiting thread never gets broken_promise.
	struct pending_call
	{
		std::promise<cmd::result> promise;
		bool settled = false;

		void set(cmd::result outcome)
		{
			settled = true;
			promise.set_value(std::move(outcome));
		}

		~pending_call()
		{
			if (!settled) promise.set_value(cmd::result::failure(cmd::codes::failed, "the command was dropped before it ran"));
		}
	};
}

// Touched on the main thread only (post() never reads it, destructor and drain both run there), so no lock.
struct cmd::main_thread_queue::state
{
	command_registry* registry = nullptr;
};

cmd::main_thread_queue::main_thread_queue(command_registry& registry, ds::task_system& tasks)
	: m_tasks(tasks)
	, m_state(std::make_shared<state>())
{
	ASSERT_MSG(registry.on_main_thread() && tasks.on_main(), "main_thread_queue: create it on the main thread");
	m_state->registry = &registry;
}

cmd::main_thread_queue::~main_thread_queue()
{
	m_state->registry = nullptr;
}

std::future<cmd::result> cmd::main_thread_queue::post(std::string id, boost::json::value args, origin from, std::uint64_t expected_generation)
{
	auto pending = std::make_shared<pending_call>();
	std::future<result> outcome = pending->promise.get_future();
	m_tasks.post_main([state = m_state, pending, id = std::move(id),
		request = call{ std::move(args), std::move(from), expected_generation }]() {
		if (state->registry == nullptr) {
			pending->set(result::failure(codes::failed, "the command queue was closed before the command ran"));
			return;
		}
		pending->set(state->registry->invoke(id, request));
	});
	return outcome;
}
