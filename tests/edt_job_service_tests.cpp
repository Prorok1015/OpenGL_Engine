#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_job_service.h>
#include <memory>
#include <stdexcept>
#include <string>

// job_service on a manual task_system: no threads, the test runs the lanes (run_one) and the main drain by hand.

namespace {

struct job_fixture
{
	job_fixture()
	{
		ds::task_system_config config;
		config.mode = ds::policy::manual;
		tasks = std::make_unique<ds::task_system>(config);
		jobs = std::make_unique<edt::job_service>(*tasks);
	}

	void run_lane(ds::lane lane = ds::lane::compute) { while (tasks->run_one(lane)) {} }
	void deliver() { tasks->drain_main(); }

	std::unique_ptr<ds::task_system> tasks;
	std::unique_ptr<edt::job_service> jobs;
	ds::lifetime owner{ ds::lifetime_option::cancel_on_destroy };
};

}

BOOST_FIXTURE_TEST_SUITE(EdtJobServiceTests, job_fixture)

BOOST_AUTO_TEST_CASE(Result_IsDeliveredOnMainDrainOnly)
{
	std::string got;
	const auto id = jobs->start(ds::lane::compute, { "work", "test" }, owner,
		[](ds::task_context&) { return std::string("done"); },
		[&got](ds::task_result<std::string>& r) { got = *r.value; });

	BOOST_TEST(jobs->is_running(id));
	run_lane();
	BOOST_TEST(got.empty());
	BOOST_TEST(jobs->is_running(id));

	deliver();
	BOOST_TEST(got == "done");
	BOOST_TEST(!jobs->is_running(id));
	BOOST_TEST(jobs->running_count() == 0u);
}

BOOST_AUTO_TEST_CASE(Progress_IsReportedWhileRunningAndUnknownAfterwards)
{
	std::optional<edt::job_progress> seen;
	edt::job_service::job_id id = 0;
	id = jobs->start(ds::lane::io, { "export", "test" }, owner,
		[this, &seen, &id](ds::task_context& ctx) {
			ctx.set_progress(0.5f, "half");
			seen = jobs->progress(id);
			return 1;
		},
		[](ds::task_result<int>&) {});

	const auto before = jobs->progress(id);
	BOOST_REQUIRE(before.has_value());
	BOOST_TEST(before->label == "export");
	BOOST_TEST(before->fraction == 0.f);

	run_lane(ds::lane::io);
	BOOST_REQUIRE(seen.has_value());
	BOOST_TEST(seen->fraction == 0.5f);
	BOOST_TEST(seen->note == "half");

	deliver();
	BOOST_TEST(!jobs->progress(id).has_value());
}

BOOST_AUTO_TEST_CASE(Cancel_BeforeStart_NeverRunsAndDeliversNothing)
{
	bool ran = false;
	bool delivered = false;
	const auto id = jobs->start(ds::lane::compute, { "work", "test" }, owner,
		[&ran](ds::task_context&) { ran = true; return 1; },
		[&delivered](ds::task_result<int>&) { delivered = true; });

	BOOST_TEST(jobs->cancel(id));
	run_lane();
	deliver();
	BOOST_TEST(!ran);
	BOOST_TEST(!delivered);
	BOOST_TEST(!jobs->is_running(id));
	BOOST_TEST(!jobs->cancel(id));
}

BOOST_AUTO_TEST_CASE(Cancel_DuringRun_StopsTheBodyAndDiscardsTheResult)
{
	bool delivered = false;
	bool stopped_seen = false;
	edt::job_service::job_id id = 0;
	id = jobs->start(ds::lane::compute, { "import", "test" }, owner,
		[this, &id, &stopped_seen](ds::task_context& ctx) {
			jobs->cancel(id);
			stopped_seen = ctx.stop.stop_requested();
			ctx.throw_if_stopped();
			return 1;
		},
		[&delivered](ds::task_result<int>&) { delivered = true; });

	run_lane();
	deliver();
	BOOST_TEST(stopped_seen);
	BOOST_TEST(!delivered);
	BOOST_TEST(!jobs->is_running(id));
}

BOOST_AUTO_TEST_CASE(Cancel_AfterBodyFinished_StillDiscardsTheResult)
{
	bool delivered = false;
	const auto id = jobs->start(ds::lane::compute, { "import", "test" }, owner,
		[](ds::task_context&) { return 1; },
		[&delivered](ds::task_result<int>&) { delivered = true; });

	run_lane();
	BOOST_TEST(jobs->cancel(id));
	deliver();
	BOOST_TEST(!delivered);
	BOOST_TEST(!jobs->is_running(id));
}

BOOST_AUTO_TEST_CASE(FailedJob_IsDeliveredWithItsError)
{
	std::string error;
	bool ok = true;
	jobs->start(ds::lane::compute, { "work", "test" }, owner,
		[](ds::task_context&) -> int { throw std::runtime_error("boom"); },
		[&](ds::task_result<int>& r) { ok = r.ok(); error = r.error; });

	run_lane();
	deliver();
	BOOST_TEST(!ok);
	BOOST_TEST(error == "boom");
}

BOOST_AUTO_TEST_CASE(OnDone_IsNotCalledAfterTheOwnerIsGone)
{
	int delivered = 0;
	{
		ds::lifetime short_lived;
		jobs->start(ds::lane::compute, { "work", "test" }, short_lived,
			[](ds::task_context&) { return 1; },
			[&delivered](ds::task_result<int>&) { ++delivered; });
		run_lane();
	}
	deliver();
	BOOST_TEST(delivered == 0);
	BOOST_TEST(jobs->running_count() == 0u);
}

BOOST_AUTO_TEST_CASE(OwnerWithCancelOnDestroy_StopsAPendingJob)
{
	bool ran = false;
	{
		ds::lifetime short_lived(ds::lifetime_option::cancel_on_destroy);
		jobs->start(ds::lane::compute, { "work", "test" }, short_lived,
			[&ran](ds::task_context&) { ran = true; return 1; },
			[](ds::task_result<int>&) {});
	}
	run_lane();
	deliver();
	BOOST_TEST(!ran);
	BOOST_TEST(jobs->running_count() == 0u);
}

BOOST_AUTO_TEST_CASE(CancelAllAndWait_LeavesNothingRunningOrDelivered)
{
	int ran = 0;
	int delivered = 0;
	for (int i = 0; i < 3; ++i) {
		jobs->start(i == 0 ? ds::lane::io : ds::lane::compute, { "work", "test" }, owner,
			[&ran](ds::task_context&) { ++ran; return 1; },
			[&delivered](ds::task_result<int>&) { ++delivered; });
	}
	// One finished but not delivered yet, one still queued.
	tasks->run_one(ds::lane::compute);
	BOOST_TEST(jobs->running_count() == 3u);

	jobs->cancel_all_and_wait();
	BOOST_TEST(jobs->running_count() == 0u);
	BOOST_TEST(ran == 1);

	run_lane();
	run_lane(ds::lane::io);
	deliver();
	BOOST_TEST(ran == 1);
	BOOST_TEST(delivered == 0);
	BOOST_TEST(tasks->snapshot().empty());
}

BOOST_AUTO_TEST_CASE(Destructor_CancelsWhatIsLeft)
{
	bool ran = false;
	jobs->start(ds::lane::compute, { "work", "test" }, owner,
		[&ran](ds::task_context&) { ran = true; return 1; },
		[](ds::task_result<int>&) {});
	jobs.reset();
	run_lane();
	deliver();
	BOOST_TEST(!ran);
}

BOOST_AUTO_TEST_SUITE_END()
