#include <boost/test/unit_test.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <snk/common/ds/ds_task_system.hpp>

namespace {

using namespace std::chrono_literals;
using ds::lane;
using ds::main_class;
using ds::task_status;

ds::task_system_config manual_config()
{
	ds::task_system_config config;
	config.mode = ds::policy::manual;
	return config;
}

// Time only moves when a test moves it.
struct fake_clock {
	std::chrono::steady_clock::time_point now{};

	std::function<std::chrono::steady_clock::time_point()> source()
	{
		return [this] { return now; };
	}
};

}

BOOST_AUTO_TEST_SUITE(TaskSystemManualTests)

BOOST_AUTO_TEST_CASE(ManualModeCreatesNoThreads)
{
	ds::task_system tasks(manual_config());
	BOOST_CHECK(tasks.mode() == ds::policy::manual);
	BOOST_TEST(tasks.thread_count(lane::compute) == 0u);
	BOOST_TEST(tasks.thread_count(lane::io) == 0u);
}

BOOST_AUTO_TEST_CASE(TasksRunOnlyWhenDriven)
{
	ds::task_system tasks(manual_config());
	int runs = 0;
	auto handle = tasks.run(lane::compute, { "t" }, [&](ds::task_context&) { ++runs; return 42; });
	BOOST_CHECK(handle.status() == task_status::pending);
	BOOST_TEST(runs == 0);

	BOOST_TEST(tasks.run_one(lane::compute));
	BOOST_TEST(runs == 1);
	BOOST_CHECK(handle.status() == task_status::ready);
	BOOST_TEST(handle.get() == 42);
	BOOST_TEST(handle.progress() == 1.f);
	BOOST_TEST(!tasks.run_one(lane::compute));
}

BOOST_AUTO_TEST_CASE(TasksOfOneLaneRunInSubmissionOrder)
{
	ds::task_system tasks(manual_config());
	std::vector<int> order;
	for (int i = 0; i < 4; ++i) {
		tasks.run(lane::compute, { "t" }, [&order, i](ds::task_context&) { order.push_back(i); });
	}
	while (tasks.run_one(lane::compute)) {}
	const std::vector<int> expected{ 0, 1, 2, 3 };
	BOOST_TEST(order == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(LanesAreIndependent)
{
	ds::task_system tasks(manual_config());
	std::string trace;
	tasks.run(lane::io, { "io" }, [&](ds::task_context&) { trace += 'i'; });
	tasks.run(lane::compute, { "c" }, [&](ds::task_context&) { trace += 'c'; });

	BOOST_TEST(tasks.run_one(lane::compute));
	BOOST_TEST(trace == "c");
	BOOST_TEST(!tasks.run_one(lane::compute));
	BOOST_TEST(tasks.run_one(lane::io));
	BOOST_TEST(trace == "ci");
}

BOOST_AUTO_TEST_CASE(BodySeesWorkerFlagOnlyWhileRunning)
{
	ds::task_system tasks(manual_config());
	bool on_worker_inside = false;
	tasks.run(lane::compute, { "t" }, [&](ds::task_context&) { on_worker_inside = ds::task_system::on_worker(); });
	BOOST_TEST(!ds::task_system::on_worker());
	tasks.run_one(lane::compute);
	BOOST_TEST(on_worker_inside);
	BOOST_TEST(!ds::task_system::on_worker());
}

BOOST_AUTO_TEST_CASE(ProgressAndTextAreReported)
{
	ds::task_system tasks(manual_config());
	float seen = -1.f;
	std::string seen_text;
	ds::task_handle<void> handle;
	handle = tasks.run(lane::compute, { "t" }, [&](ds::task_context& ctx) {
		ctx.set_progress(0.25f, "quarter");
		seen = handle.progress();
		seen_text = handle.progress_text();
		ctx.set_progress(2.f);
	});
	tasks.run_one(lane::compute);
	BOOST_TEST(seen == 0.25f);
	BOOST_TEST(seen_text == "quarter");
	BOOST_TEST(handle.progress() == 1.f);
	BOOST_TEST(handle.progress_text() == "quarter");
}

BOOST_AUTO_TEST_CASE(ExceptionBecomesErrorValue)
{
	ds::task_system tasks(manual_config());
	auto handle = tasks.run(lane::compute, { "t" }, [](ds::task_context&) -> int { throw std::runtime_error("boom"); });
	tasks.run_one(lane::compute);
	BOOST_CHECK(handle.status() == task_status::error);
	BOOST_TEST(handle.error() == "boom");
}

BOOST_AUTO_TEST_CASE(CancelBeforeStartNeverRunsTheBody)
{
	ds::task_system tasks(manual_config());
	bool ran = false;
	auto handle = tasks.run(lane::compute, { "t" }, [&](ds::task_context&) { ran = true; });
	handle.cancel();
	BOOST_CHECK(handle.status() == task_status::cancelled);

	tasks.run_one(lane::compute);
	BOOST_TEST(!ran);
	BOOST_CHECK(handle.status() == task_status::cancelled);
}

BOOST_AUTO_TEST_CASE(CancelDuringExecutionIsCooperative)
{
	ds::task_system tasks(manual_config());
	bool saw_stop = false;
	bool finished_body = false;
	ds::task_handle<void> handle;
	handle = tasks.run(lane::compute, { "t" }, [&](ds::task_context& ctx) {
		handle.cancel();
		BOOST_CHECK(handle.status() == task_status::running);
		saw_stop = ctx.stop.stop_requested();
		ctx.throw_if_stopped();
		finished_body = true;
	});
	tasks.run_one(lane::compute);
	BOOST_TEST(saw_stop);
	BOOST_TEST(!finished_body);
	BOOST_CHECK(handle.status() == task_status::cancelled);
}

BOOST_AUTO_TEST_CASE(BodyThatIgnoresCancelIsStillReportedCancelled)
{
	ds::task_system tasks(manual_config());
	ds::task_handle<int> handle;
	handle = tasks.run(lane::compute, { "t" }, [&](ds::task_context&) {
		handle.cancel();
		return 7;
	});
	tasks.run_one(lane::compute);
	BOOST_CHECK(handle.status() == task_status::cancelled);
}

BOOST_AUTO_TEST_CASE(ThenMainRunsOnlyWhenTheMainQueueIsDrained)
{
	ds::task_system tasks(manual_config());
	int callbacks = 0;
	int value = 0;
	auto handle = tasks.run(lane::compute, { "t" }, [](ds::task_context&) { return 5; });
	handle.then_main([&](ds::task_result<int>& result) {
		++callbacks;
		BOOST_TEST(result.ok());
		value = *result.value;
	});

	tasks.run_one(lane::compute);
	BOOST_TEST(callbacks == 0);
	BOOST_TEST(tasks.drain_main() == 1u);
	BOOST_TEST(callbacks == 1);
	BOOST_TEST(value == 5);
}

BOOST_AUTO_TEST_CASE(ThenMainAfterCompletionStillGoesThroughTheQueue)
{
	ds::task_system tasks(manual_config());
	auto handle = tasks.run(lane::compute, { "t" }, [](ds::task_context&) {});
	tasks.run_one(lane::compute);

	bool called = false;
	handle.then_main([&](ds::task_result<void>& result) { called = result.ok(); });
	BOOST_TEST(!called);
	tasks.drain_main();
	BOOST_TEST(called);
}

BOOST_AUTO_TEST_CASE(ThenMainReceivesErrorAndCancellation)
{
	ds::task_system tasks(manual_config());
	auto failing = tasks.run(lane::compute, { "f" }, [](ds::task_context&) -> int { throw std::runtime_error("bad"); });
	auto cancelled = tasks.run(lane::compute, { "c" }, [](ds::task_context&) { return 1; });
	std::string error;
	task_status failing_status = task_status::pending;
	task_status cancelled_status = task_status::pending;
	failing.then_main([&](ds::task_result<int>& r) { error = r.error; failing_status = r.status; });
	cancelled.then_main([&](ds::task_result<int>& r) { cancelled_status = r.status; });

	cancelled.cancel();
	while (tasks.run_one(lane::compute)) {}
	tasks.drain_main();
	BOOST_TEST(error == "bad");
	BOOST_CHECK(failing_status == task_status::error);
	BOOST_CHECK(cancelled_status == task_status::cancelled);
}

BOOST_AUTO_TEST_CASE(GeneralBudgetStopsTheDrainOnTheInjectedClock)
{
	fake_clock clock;
	auto config = manual_config();
	config.clock = clock.source();
	ds::task_system tasks(config);

	int ran = 0;
	for (int i = 0; i < 5; ++i) {
		tasks.post_main([&] { ++ran; clock.now += 1ms; });
	}
	BOOST_TEST(tasks.drain_main(2ms) == 2u);
	BOOST_TEST(ran == 2);
	BOOST_TEST(tasks.drain_main(2ms) == 2u);
	BOOST_TEST(tasks.drain_main(2ms) == 1u);
	BOOST_TEST(ran == 5);
	BOOST_TEST(tasks.drain_main(2ms) == 0u);
}

BOOST_AUTO_TEST_CASE(CheapItemsAllFitInOneBudget)
{
	fake_clock clock;
	auto config = manual_config();
	config.clock = clock.source();
	ds::task_system tasks(config);

	int ran = 0;
	for (int i = 0; i < 5; ++i) {
		tasks.post_main([&] { ++ran; });
	}
	BOOST_TEST(tasks.drain_main(2ms) == 5u);
	BOOST_TEST(ran == 5);
}

BOOST_AUTO_TEST_CASE(AtLeastOneGeneralItemRunsPerFrameEvenWithoutBudget)
{
	fake_clock clock;
	auto config = manual_config();
	config.clock = clock.source();
	ds::task_system tasks(config);

	int ran = 0;
	for (int i = 0; i < 3; ++i) {
		tasks.post_main([&] { ++ran; clock.now += 10ms; });
	}
	BOOST_TEST(tasks.drain_main(0ms) == 1u);
	BOOST_TEST(tasks.drain_main(0ms) == 1u);
	BOOST_TEST(tasks.drain_main(0ms) == 1u);
	BOOST_TEST(ran == 3);
}

BOOST_AUTO_TEST_CASE(ConfiguredBudgetIsUsedByDrainWithoutArguments)
{
	fake_clock clock;
	auto config = manual_config();
	config.clock = clock.source();
	config.main_budget = 3ms;
	ds::task_system tasks(config);

	for (int i = 0; i < 5; ++i) {
		tasks.post_main([&] { clock.now += 1ms; });
	}
	BOOST_TEST(tasks.drain_main() == 3u);
	tasks.set_main_budget(1ms);
	BOOST_TEST(tasks.drain_main() == 1u);
}

BOOST_AUTO_TEST_CASE(GpuUploadIsLimitedByCountNotByTime)
{
	fake_clock clock;
	auto config = manual_config();
	config.clock = clock.source();
	config.gpu_upload_per_frame = 2;
	ds::task_system tasks(config);

	int uploads = 0;
	int general = 0;
	for (int i = 0; i < 5; ++i) {
		tasks.post_main([&] { ++uploads; clock.now += 50ms; }, main_class::gpu_upload);
	}
	tasks.post_main([&] { ++general; });

	BOOST_TEST(tasks.drain_main(1ms) == 3u);
	BOOST_TEST(uploads == 2);
	BOOST_TEST(general == 1);
	BOOST_TEST(tasks.pending_main(main_class::gpu_upload) == 3u);

	tasks.set_gpu_upload_limit(10);
	BOOST_TEST(tasks.drain_main(1ms) == 3u);
	BOOST_TEST(uploads == 5);
}

BOOST_AUTO_TEST_CASE(ItemPostedFromAMainCallbackWaitsForTheBudget)
{
	fake_clock clock;
	auto config = manual_config();
	config.clock = clock.source();
	ds::task_system tasks(config);

	std::string trace;
	tasks.post_main([&] {
		trace += 'a';
		clock.now += 5ms;
		tasks.post_main([&] { trace += 'b'; });
	});
	BOOST_TEST(tasks.drain_main(2ms) == 1u);
	BOOST_TEST(trace == "a");
	BOOST_TEST(tasks.drain_main(2ms) == 1u);
	BOOST_TEST(trace == "ab");
}

BOOST_AUTO_TEST_CASE(ThrowingMainItemDoesNotStopTheDrain)
{
	ds::task_system tasks(manual_config());
	int ran = 0;
	tasks.post_main([] { throw std::runtime_error("bad item"); });
	tasks.post_main([&] { ++ran; });
	BOOST_TEST(tasks.drain_main(10ms) == 2u);
	BOOST_TEST(ran == 1);
}

BOOST_AUTO_TEST_CASE(ShutdownDropsTheMainQueueWithoutCallingIt)
{
	bool posted_called = false;
	bool upload_called = false;
	bool then_called = false;
	ds::task_handle<int> pending;
	ds::task_handle<int> finished;
	{
		ds::task_system tasks(manual_config());
		tasks.post_main([&] { posted_called = true; });
		tasks.post_main([&] { upload_called = true; }, main_class::gpu_upload);

		finished = tasks.run(lane::compute, { "done" }, [](ds::task_context&) { return 1; });
		tasks.run_one(lane::compute);
		finished.then_main([&](ds::task_result<int>&) { then_called = true; });

		pending = tasks.run(lane::compute, { "never" }, [](ds::task_context&) { return 2; });
	}
	BOOST_TEST(!posted_called);
	BOOST_TEST(!upload_called);
	BOOST_TEST(!then_called);
	BOOST_CHECK(finished.status() == task_status::ready);
	BOOST_CHECK(pending.status() == task_status::cancelled);
}

BOOST_AUTO_TEST_CASE(PostMainAfterShutdownIsHarmlessForSurvivingHandles)
{
	ds::task_handle<int> handle;
	{
		ds::task_system tasks(manual_config());
		handle = tasks.run(lane::compute, { "t" }, [](ds::task_context&) { return 1; });
	}
	bool called = false;
	handle.then_main([&](ds::task_result<int>&) { called = true; });
	BOOST_TEST(!called);
	BOOST_CHECK(handle.status() == task_status::cancelled);
}

BOOST_AUTO_TEST_CASE(WhenAllRunsAfterEveryInputWithoutBlocking)
{
	ds::task_system tasks(manual_config());
	std::string trace;
	auto a = tasks.run(lane::compute, { "a" }, [&](ds::task_context&) { trace += 'a'; });
	auto b = tasks.run(lane::io, { "b" }, [&](ds::task_context&) { trace += 'b'; });
	const ds::task_handle_base inputs[] = { a, b };
	auto all = tasks.when_all(inputs, lane::compute, [&](ds::task_context&) { trace += 'X'; });
	BOOST_CHECK(all.status() == task_status::pending);

	tasks.run_one(lane::compute);
	BOOST_TEST(trace == "a");
	BOOST_TEST(!tasks.run_one(lane::compute));
	tasks.run_one(lane::io);
	BOOST_TEST(trace == "ab");
	BOOST_TEST(tasks.run_one(lane::compute));
	BOOST_TEST(trace == "abX");
	BOOST_CHECK(all.status() == task_status::ready);
}

BOOST_AUTO_TEST_CASE(WhenAllOverFinishedInputsIsQueuedImmediately)
{
	ds::task_system tasks(manual_config());
	auto a = tasks.run(lane::compute, { "a" }, [](ds::task_context&) {});
	tasks.run_one(lane::compute);

	bool ran = false;
	const ds::task_handle_base inputs[] = { a };
	auto all = tasks.when_all(inputs, lane::compute, [&](ds::task_context&) { ran = true; });
	BOOST_TEST(!ran);
	tasks.run_one(lane::compute);
	BOOST_TEST(ran);
	BOOST_CHECK(all.status() == task_status::ready);
}

BOOST_AUTO_TEST_CASE(WhenAllWithoutInputsRuns)
{
	ds::task_system tasks(manual_config());
	bool ran = false;
	auto all = tasks.when_all({}, lane::compute, [&](ds::task_context&) { ran = true; });
	tasks.run_one(lane::compute);
	BOOST_TEST(ran);
	BOOST_CHECK(all.status() == task_status::ready);
}

BOOST_AUTO_TEST_CASE(WhenAllWithAFailedInputFailsAndSkipsTheBody)
{
	ds::task_system tasks(manual_config());
	bool ran = false;
	auto good = tasks.run(lane::compute, { "good" }, [](ds::task_context&) {});
	auto bad = tasks.run(lane::compute, { "bad" }, [](ds::task_context&) { throw std::runtime_error("disk gone"); });
	const ds::task_handle_base inputs[] = { good, bad };
	auto all = tasks.when_all(inputs, lane::compute, [&](ds::task_context&) { ran = true; });

	bool continuation_saw_error = false;
	all.then_main([&](ds::task_result<void>& result) {
		continuation_saw_error = result.status == task_status::error;
	});

	while (tasks.run_one(lane::compute)) {}
	tasks.drain_main();
	BOOST_TEST(!ran);
	BOOST_CHECK(all.status() == task_status::error);
	BOOST_TEST(all.error().find("bad") != std::string::npos);
	BOOST_TEST(all.error().find("disk gone") != std::string::npos);
	BOOST_TEST(continuation_saw_error);
}

BOOST_AUTO_TEST_CASE(WhenAllWithACancelledInputIsCancelled)
{
	ds::task_system tasks(manual_config());
	bool ran = false;
	auto a = tasks.run(lane::compute, { "a" }, [](ds::task_context&) {});
	auto b = tasks.run(lane::compute, { "b" }, [](ds::task_context&) {});
	const ds::task_handle_base inputs[] = { a, b };
	auto all = tasks.when_all(inputs, lane::compute, [&](ds::task_context&) { ran = true; });

	b.cancel();
	while (tasks.run_one(lane::compute)) {}
	BOOST_TEST(!ran);
	BOOST_CHECK(all.status() == task_status::cancelled);
}

BOOST_AUTO_TEST_CASE(CancelledWhenAllNeverRuns)
{
	ds::task_system tasks(manual_config());
	bool ran = false;
	auto a = tasks.run(lane::compute, { "a" }, [](ds::task_context&) {});
	const ds::task_handle_base inputs[] = { a };
	auto all = tasks.when_all(inputs, lane::compute, [&](ds::task_context&) { ran = true; });

	all.cancel();
	while (tasks.run_one(lane::compute)) {}
	BOOST_TEST(!ran);
	BOOST_CHECK(all.status() == task_status::cancelled);
}

BOOST_AUTO_TEST_CASE(WaitInManualModeDrivesTheQueue)
{
	ds::task_system tasks(manual_config());
	auto a = tasks.run(lane::compute, { "a" }, [](ds::task_context&) { return 1; });
	auto b = tasks.run(lane::io, { "b" }, [](ds::task_context&) { return 2; });
	const ds::task_handle_base inputs[] = { a, b };
	auto all = tasks.when_all(inputs, lane::compute, [](ds::task_context&) {});

	all.wait();
	BOOST_CHECK(all.status() == task_status::ready);
	BOOST_TEST(a.get() + b.get() == 3);
}

BOOST_AUTO_TEST_CASE(SnapshotListsOnlyVisibleUnfinishedTasks)
{
	ds::task_system tasks(manual_config());
	tasks.run(lane::compute, { "shown", "import", true }, [](ds::task_context& ctx) { ctx.set_progress(0.5f, "half"); });
	tasks.run(lane::io, { "hidden", "internal", false }, [](ds::task_context&) {});
	auto second = tasks.run(lane::compute, { "second", "export", true }, [](ds::task_context&) {});

	auto infos = tasks.snapshot();
	BOOST_REQUIRE(infos.size() == 2u);
	BOOST_TEST(infos[0].name == "shown");
	BOOST_TEST(infos[0].category == "import");
	BOOST_CHECK(infos[0].status == task_status::pending);
	BOOST_TEST(infos[1].name == "second");

	tasks.run_one(lane::compute);
	infos = tasks.snapshot();
	BOOST_REQUIRE(infos.size() == 1u);
	BOOST_TEST(infos[0].name == "second");

	second.cancel();
	BOOST_TEST(tasks.snapshot().empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(TaskSystemLifetimeTests)

BOOST_AUTO_TEST_CASE(CallbackRunsWhileOwnerIsAlive)
{
	ds::task_system tasks(manual_config());
	ds::lifetime owner;
	int calls = 0;
	auto handle = tasks.run(lane::compute, { "t" }, [](ds::task_context&) { return 7; });
	handle.then_main(owner, [&](ds::task_result<int>& result) {
		++calls;
		BOOST_CHECK(result.ok());
		BOOST_TEST(*result.value == 7);
	});
	tasks.run_one(lane::compute);
	tasks.drain_main();
	BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_CASE(CallbackIsSkippedAfterOwnerDestroyed)
{
	ds::task_system tasks(manual_config());
	int calls = 0;
	auto owner = std::make_unique<ds::lifetime>();
	auto handle = tasks.run(lane::compute, { "t" }, [](ds::task_context&) {});
	handle.then_main(*owner, [&](ds::task_result<void>&) { ++calls; });
	tasks.run_one(lane::compute);
	BOOST_TEST(tasks.pending_main(main_class::general) == 1u);

	owner.reset();
	tasks.drain_main();
	BOOST_TEST(calls == 0);
	BOOST_TEST(tasks.pending_main(main_class::general) == 0u);
}

BOOST_AUTO_TEST_CASE(CallbackIsSkippedWhenOwnerDiesBeforeTheTaskFinishes)
{
	ds::task_system tasks(manual_config());
	int calls = 0;
	auto owner = std::make_unique<ds::lifetime>();
	auto handle = tasks.run(lane::compute, { "t" }, [](ds::task_context&) {});
	handle.then_main(*owner, [&](ds::task_result<void>&) { ++calls; });
	owner.reset();
	tasks.run_one(lane::compute);
	tasks.drain_main();
	BOOST_TEST(calls == 0);
	BOOST_CHECK(handle.status() == task_status::ready); // no cancel_on_destroy: the task still completes
}

BOOST_AUTO_TEST_CASE(CallbackOfFinishedTaskIsSkippedAfterOwnerDestroyed)
{
	ds::task_system tasks(manual_config());
	int calls = 0;
	auto handle = tasks.run(lane::compute, { "t" }, [](ds::task_context&) {});
	tasks.run_one(lane::compute);
	{
		ds::lifetime owner;
		handle.then_main(owner, [&](ds::task_result<void>&) { ++calls; });
	}
	tasks.drain_main();
	BOOST_TEST(calls == 0);
}

BOOST_AUTO_TEST_CASE(PlainThenMainIgnoresLifetimes)
{
	ds::task_system tasks(manual_config());
	int calls = 0;
	auto handle = tasks.run(lane::compute, { "t" }, [](ds::task_context&) {});
	handle.then_main([&](ds::task_result<void>&) { ++calls; });
	tasks.run_one(lane::compute);
	tasks.drain_main();
	BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_CASE(CancelOnDestroyCancelsPendingTasks)
{
	ds::task_system tasks(manual_config());
	int runs = 0;
	ds::task_handle<void> handle;
	{
		ds::lifetime owner(ds::lifetime_option::cancel_on_destroy);
		handle = tasks.run(lane::compute, { "t" }, owner, [&](ds::task_context&) { ++runs; });
		BOOST_CHECK(handle.status() == task_status::pending);
	}
	BOOST_CHECK(handle.status() == task_status::cancelled);
	while (tasks.run_one(lane::compute)) {}
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(CancelOnDestroyAsksRunningTasksToStop)
{
	ds::task_system tasks(manual_config());
	auto owner = std::make_unique<ds::lifetime>(ds::lifetime_option::cancel_on_destroy);
	bool finished_body = false;
	auto handle = tasks.run(lane::compute, { "t" }, *owner, [&](ds::task_context& ctx) {
		owner.reset(); // the owner dies while its task is running
		ctx.throw_if_stopped();
		finished_body = true;
	});
	tasks.run_one(lane::compute);
	BOOST_TEST(!finished_body);
	BOOST_CHECK(handle.status() == task_status::cancelled);
}

BOOST_AUTO_TEST_CASE(WithoutCancelOnDestroyTasksOutliveTheOwner)
{
	ds::task_system tasks(manual_config());
	int runs = 0;
	{
		ds::lifetime owner;
		tasks.run(lane::compute, { "t" }, owner, [&](ds::task_context&) { ++runs; });
	}
	tasks.run_one(lane::compute);
	BOOST_TEST(runs == 1);
}

BOOST_AUTO_TEST_CASE(CancelTasksOnlyTouchesBoundTasks)
{
	ds::task_system tasks(manual_config());
	ds::lifetime owner;
	auto bound = tasks.run(lane::compute, { "bound" }, owner, [](ds::task_context&) {});
	auto other = tasks.run(lane::compute, { "other" }, [](ds::task_context&) {});
	owner.cancel_tasks();
	BOOST_CHECK(bound.status() == task_status::cancelled);
	BOOST_CHECK(other.status() == task_status::pending);
}

BOOST_AUTO_TEST_CASE(WaitIdleReturnsOnlyAfterBoundTasksFinish)
{
	ds::task_system tasks(manual_config());
	ds::lifetime owner;
	int runs = 0;
	auto a = tasks.run(lane::compute, { "a" }, owner, [&](ds::task_context&) { ++runs; });
	auto b = tasks.run(lane::io, { "b" }, owner, [&](ds::task_context&) { ++runs; });
	BOOST_CHECK(!a.done());
	BOOST_CHECK(!b.done());

	tasks.wait_idle(owner); // manual mode: drives the queues until the bound tasks are done
	BOOST_CHECK(a.done());
	BOOST_CHECK(b.done());
	BOOST_TEST(runs == 2);
}

BOOST_AUTO_TEST_CASE(WaitIdleWaitsForChildrenBoundByBoundTasks)
{
	ds::task_system tasks(manual_config());
	ds::lifetime owner;
	int child_runs = 0;
	ds::task_handle<void> child;
	tasks.run(lane::compute, { "parent" }, owner, [&](ds::task_context&) {
		child = tasks.run(lane::io, { "child" }, owner, [&](ds::task_context&) { ++child_runs; });
	});
	tasks.wait_idle(owner);
	BOOST_TEST(child_runs == 1);
	BOOST_CHECK(child.done());
}

BOOST_AUTO_TEST_CASE(WaitIdleLeavesUnboundTasksAlone)
{
	ds::task_system tasks(manual_config());
	ds::lifetime owner;
	std::string trace;
	tasks.run(lane::compute, { "unbound" }, [&](ds::task_context&) { trace += 'u'; });
	tasks.run(lane::compute, { "bound" }, owner, [&](ds::task_context&) { trace += 'b'; });
	tasks.wait_idle(owner);
	BOOST_TEST(trace == "ub"); // FIFO queue: the bound one is behind the unbound one
}

BOOST_AUTO_TEST_CASE(WaitIdleAfterCancelDoesNotRunTheBodies)
{
	ds::task_system tasks(manual_config());
	ds::lifetime owner;
	int runs = 0;
	auto handle = tasks.run(lane::compute, { "t" }, owner, [&](ds::task_context&) { ++runs; });
	owner.cancel_tasks();
	tasks.wait_idle(owner);
	BOOST_TEST(runs == 0);
	BOOST_CHECK(handle.status() == task_status::cancelled);
}

BOOST_AUTO_TEST_CASE(CancelReleasesTheCapturesOfAQueuedTask)
{
	ds::task_system tasks(manual_config());
	ds::lifetime owner;
	auto capture = std::make_shared<int>(0);
	const std::weak_ptr<int> watched = capture;
	auto handle = tasks.run(lane::compute, { "t" }, owner, [capture](ds::task_context&) {});
	capture.reset();
	BOOST_TEST(!watched.expired()); // the queued task holds it

	owner.cancel_tasks();
	tasks.wait_idle(owner);
	BOOST_TEST(watched.expired()); // gone although the task is still in the queue
	BOOST_CHECK(handle.status() == task_status::cancelled);
}

BOOST_AUTO_TEST_CASE(WhenAllTakesTypedHandles)
{
	ds::task_system tasks(manual_config());
	std::vector<ds::task_handle<int>> handles;
	for (int i = 0; i < 3; ++i) {
		handles.push_back(tasks.run(lane::compute, { "t" }, [i](ds::task_context&) { return i; }));
	}
	bool ran = false;
	auto all = tasks.when_all(handles, lane::compute, [&](ds::task_context&) { ran = true; });
	while (tasks.run_one(lane::compute)) {}
	BOOST_CHECK(all.status() == task_status::ready);
	BOOST_TEST(ran);
}

BOOST_AUTO_TEST_CASE(WhenAllTakesAnInitializerList)
{
	ds::task_system tasks(manual_config());
	auto a = tasks.run(lane::compute, { "a" }, [](ds::task_context&) { return 1; });
	auto b = tasks.run(lane::io, { "b" }, [](ds::task_context&) {});
	bool ran = false;
	auto all = tasks.when_all({ a, b }, lane::compute, [&](ds::task_context&) { ran = true; });
	BOOST_CHECK(all.status() == task_status::pending);
	while (tasks.run_one(lane::compute) || tasks.run_one(lane::io)) {}
	BOOST_CHECK(all.status() == task_status::ready);
	BOOST_TEST(ran);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(TaskSystemThreadedTests)

// Real threads: meant for the TSan job. Everything the callbacks touch is main-thread data, so a
// race here is a task_system bug.
BOOST_AUTO_TEST_CASE(ThenMainRunsOnTheMainThreadAfterCompletion)
{
	ds::task_system_config config;
	config.compute_threads = 3;
	ds::task_system tasks(config);
	BOOST_TEST(tasks.thread_count(lane::compute) == 3u);
	BOOST_TEST(tasks.thread_count(lane::io) == 2u);

	constexpr int count = 200;
	const auto main_id = std::this_thread::get_id();
	std::vector<ds::task_handle<int>> handles;
	handles.reserve(count);

	int completed = 0;
	long sum = 0;
	bool wrong_thread = false;
	for (int i = 0; i < count; ++i) {
		auto handle = tasks.run(i % 5 == 0 ? lane::io : lane::compute, { "t" }, [i](ds::task_context& ctx) {
			ctx.set_progress(0.5f);
			return i;
		});
		handle.then_main([&](ds::task_result<int>& result) {
			wrong_thread = wrong_thread || std::this_thread::get_id() != main_id;
			++completed;
			if (result.ok()) {
				sum += *result.value;
			}
		});
		handles.push_back(std::move(handle));
	}

	const auto deadline = std::chrono::steady_clock::now() + 20s;
	while (completed < count && std::chrono::steady_clock::now() < deadline) {
		tasks.drain_main(1ms);
		std::this_thread::sleep_for(100us);
	}
	BOOST_TEST(completed == count);
	BOOST_TEST(!wrong_thread);
	BOOST_TEST(sum == static_cast<long>(count) * (count - 1) / 2);
}

BOOST_AUTO_TEST_CASE(CancelRacesWithCompletionWithoutLosingTheCallback)
{
	ds::task_system_config config;
	config.compute_threads = 2;
	ds::task_system tasks(config);

	constexpr int count = 200;
	int callbacks = 0;
	std::vector<ds::task_handle<int>> handles;
	handles.reserve(count);
	for (int i = 0; i < count; ++i) {
		auto handle = tasks.run(lane::compute, { "t" }, [i](ds::task_context&) { return i; });
		handle.then_main([&](ds::task_result<int>&) { ++callbacks; });
		handles.push_back(std::move(handle));
	}
	for (int i = 0; i < count; i += 2) {
		handles[static_cast<std::size_t>(i)].cancel();
	}

	const auto deadline = std::chrono::steady_clock::now() + 20s;
	while (callbacks < count && std::chrono::steady_clock::now() < deadline) {
		tasks.drain_main(1ms);
		std::this_thread::sleep_for(100us);
	}
	BOOST_TEST(callbacks == count);
	for (const auto& handle : handles) {
		BOOST_CHECK(handle.done());
	}
}

BOOST_AUTO_TEST_CASE(WaitAndWhenAllWorkOnThePool)
{
	ds::task_system tasks{ ds::task_system_config{} };
	auto a = tasks.run(lane::compute, { "a" }, [](ds::task_context&) { return 20; });
	auto b = tasks.run(lane::io, { "b" }, [](ds::task_context&) { return 22; });
	const ds::task_handle_base inputs[] = { a, b };
	std::atomic<bool> ran{ false };
	auto all = tasks.when_all(inputs, lane::compute, [&](ds::task_context&) { ran = true; });
	all.wait();
	BOOST_CHECK(all.status() == task_status::ready);
	BOOST_TEST(ran.load());
	BOOST_TEST(a.get() + b.get() == 42);
}

BOOST_AUTO_TEST_CASE(ShutdownCancelsRunningAndQueuedTasks)
{
	std::atomic<bool> started{ false };
	std::atomic<bool> saw_stop{ false };
	ds::task_handle<void> running;
	ds::task_handle<void> queued;
	{
		ds::task_system_config config;
		config.compute_threads = 1;
		ds::task_system tasks(config);
		running = tasks.run(lane::compute, { "running" }, [&](ds::task_context& ctx) {
			started = true;
			while (!ctx.stop.stop_requested()) {
				std::this_thread::sleep_for(100us);
			}
			saw_stop = true;
		});
		queued = tasks.run(lane::compute, { "queued" }, [](ds::task_context&) {});
		while (!started) {
			std::this_thread::sleep_for(100us);
		}
	}
	BOOST_TEST(saw_stop.load());
	BOOST_CHECK(running.status() == task_status::cancelled);
	BOOST_CHECK(queued.status() == task_status::cancelled);
}

BOOST_AUTO_TEST_CASE(OwnerDestroyedOnMainWhileTaskCompletesOnWorker)
{
	struct owner_t {
		explicit owner_t(ds::lifetime_option option) : life(option) {}
		~owner_t() { *destroyed = true; }

		std::shared_ptr<bool> destroyed = std::make_shared<bool>(false); // main-thread data
		ds::lifetime life;
	};

	struct probe_t {
		explicit probe_t(std::atomic<int>& dropped_count) : dropped(dropped_count) {}
		~probe_t() { dropped += delivered ? 0 : 1; }

		std::atomic<int>& dropped;
		bool delivered = false; // main-thread data, published to the destructor by the shared_ptr count
	};

	constexpr int rounds = 300;
	int delivered = 0;
	int after_death = 0;
	std::atomic<int> dropped{ 0 };
	{
		ds::task_system_config config;
		config.compute_threads = 3;
		ds::task_system tasks(config);

		std::vector<ds::task_handle<int>> handles;
		handles.reserve(rounds);
		for (int i = 0; i < rounds; ++i) {
			auto owner = std::make_unique<owner_t>(i % 2 == 0 ? ds::lifetime_option::cancel_on_destroy
				: ds::lifetime_option::none);
			auto handle = tasks.run(lane::compute, { "t" }, owner->life, [](ds::task_context& ctx) {
				ctx.throw_if_stopped();
				return 1;
			});
			handle.then_main(owner->life, [&, destroyed = owner->destroyed, probe = std::make_shared<probe_t>(dropped)](ds::task_result<int>&) {
				probe->delivered = true;
				++delivered;
				after_death += *destroyed ? 1 : 0;
			});
			if (i == 0) {
				// Deterministic delivery: keep the owner alive until its continuation has really run, so
				// `delivered > 0` does not depend on scheduling. The other rounds keep racing the owner's death.
				handle.wait();
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (delivered == 0 && std::chrono::steady_clock::now() < deadline) {
					tasks.drain_main(1ms);
				}
				if (delivered == 0) {
					BOOST_FAIL("continuation of the first round was not delivered to the main queue within 5 s");
				}
			}
			if (i % 3 == 0) {
				tasks.drain_main(1ms);
			}
			if (i % 4 == 0) {
				std::this_thread::yield();
			}
			owner.reset(); // races with a worker finishing the task
			handles.push_back(std::move(handle));
		}
		for (const auto& handle : handles) {
			handle.wait();
		}
		handles.clear();
		tasks.drain_main(1ms);

		// `wait()` returns when the task is done, which is published before the worker has handed its
		// continuations to the main queue, and drain_main(budget) may stop early. A continuation can thus
		// be legitimately still in flight here; the destructor joins the workers and destroys whatever
		// is left in the queues, so every probe is accounted for once `tasks` is gone.
	}
	BOOST_TEST(after_death == 0);
	BOOST_TEST(delivered > 0);
	BOOST_TEST(delivered + dropped.load() == rounds);
}

BOOST_AUTO_TEST_CASE(TryRunInline_ClaimsAQueuedTaskForABlockedWorker)
{
	ds::task_system_config config;
	config.compute_threads = 1;
	ds::task_system tasks(config);

	std::atomic<bool> first_started{ false };
	std::atomic<bool> claimed{ false };
	std::atomic<bool> claimed_again{ true };
	std::atomic<bool> second_ran_inside{ false };
	std::atomic<int> second_runs{ 0 };
	std::atomic<std::uint64_t> second_id{ 0 };
	auto first = tasks.run(lane::compute, {}, [&](ds::task_context&) {
		first_started = true;
		while (second_id.load() == 0) std::this_thread::sleep_for(1ms);
		// The only pool thread is this one: the queued task can only run if the waiter takes it.
		claimed = tasks.try_run_inline(second_id.load());
		second_ran_inside = second_runs.load() == 1;
		claimed_again = tasks.try_run_inline(second_id.load());
	});
	while (!first_started.load()) std::this_thread::sleep_for(1ms);
	auto second = tasks.run(lane::compute, {}, [&](ds::task_context&) { ++second_runs; });
	second_id = second.id();
	first.wait();
	second.wait();
	BOOST_TEST(claimed.load());
	BOOST_TEST(second_ran_inside.load());
	BOOST_TEST(!claimed_again.load()); // done already
	BOOST_TEST(second_runs.load() == 1); // its own pool job found it claimed
	BOOST_TEST((second.status() == task_status::ready));
	BOOST_TEST(!tasks.try_run_inline(987654321)); // unknown id
}

BOOST_AUTO_TEST_SUITE_END()
