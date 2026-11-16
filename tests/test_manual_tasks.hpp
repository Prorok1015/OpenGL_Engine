#pragma once
// ds::task_system without threads: a test decides when queued work runs. Stands in for the removed inline
// adapter worker: a resource_system built over it loads nothing until the test (or a get_sync()) drives the lanes.
#include <snk/common/ds/ds_task_system.hpp>
#include <chrono>
#include <exception>

namespace test_util {

inline ds::task_system_config manual_tasks()
{
	ds::task_system_config config;
	config.mode = ds::policy::manual;
	return config;
}

// Runs `fn` now as a compute task on this thread, as a pool thread would (ds::task_system::on_worker() inside), so
// what may not run on the main thread outside a task (resource_system::read_now) can be called; rethrows what it threw.
template<class F>
void in_task(ds::task_system& tasks, F&& fn)
{
	std::exception_ptr failed;
	[[maybe_unused]] auto handle = tasks.run(ds::lane::compute, ds::task_desc{ .name = "test", .category = "test" },
		[&fn, &failed](ds::task_context&) {
			try {
				fn();
			} catch (...) {
				failed = std::current_exception();
			}
		});
	while (tasks.run_one(ds::lane::compute)) {}
	if (failed) std::rethrow_exception(failed);
}

// Runs everything that is queued, including what the executed work queues in turn: tasks on both lanes,
// then the main-thread queue (resource callbacks, watcher notifications).
inline void pump(ds::task_system& tasks)
{
	for (bool ran = true; ran;) {
		ran = false;
		while (tasks.run_one(ds::lane::compute) || tasks.run_one(ds::lane::io)) {
			ran = true;
		}
		if (tasks.drain_main(std::chrono::hours(1)) > 0) {
			ran = true;
		}
	}
}

}
