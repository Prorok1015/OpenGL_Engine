#pragma once
// ds::task_system without threads: a test decides when queued work runs. Stands in for the removed inline
// adapter worker: a resource_system built over it loads nothing until the test (or a get_sync()) drives the lanes.
#include <snk/common/ds/ds_task_system.hpp>
#include <chrono>

namespace test_util {

inline ds::task_system_config manual_tasks()
{
	ds::task_system_config config;
	config.mode = ds::policy::manual;
	return config;
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
