#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_service_commands.h>
#include <boost/json/parse.hpp>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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

namespace {

namespace json = boost::json;

// job_service with editor_events, on a manual task system. `log` collects what the events said, one line each.
struct job_events_fixture
{
	job_events_fixture()
	{
		ds::task_system_config config;
		config.mode = ds::policy::manual;
		tasks = std::make_unique<ds::task_system>(config);
		jobs = std::make_unique<edt::job_service>(*tasks, &events);
		connections.push_back(events.job_started.subscribe([this](std::uint64_t id, const std::string& label) {
			log.push_back(std::format("start:{}:{}", id, label));
		}));
		connections.push_back(events.job_progress.subscribe([this](std::uint64_t id, float fraction, const std::string& note) {
			log.push_back(std::format("progress:{}:{}:{}", id, fraction, note));
		}));
		connections.push_back(events.job_finished.subscribe([this](std::uint64_t id, edt::job_status status, const std::string& error) {
			log.push_back(std::format("finish:{}:{}:{}", id, edt::job_status_name(status), error));
		}));
	}

	~job_events_fixture()
	{
		jobs.reset();
		connections.clear();
	}

	void run_lane(ds::lane lane = ds::lane::compute) { while (tasks->run_one(lane)) {} }
	void deliver() { tasks->drain_main(); }

	template<class F>
	edt::job_service::job_id start(std::string label, F&& work, ds::lane lane = ds::lane::compute)
	{
		return jobs->start(lane, { label, "test" }, owner, std::forward<F>(work),
			std::function<void(ds::task_result<std::invoke_result_t<F&, ds::task_context&>>&)>{});
	}

	edt::editor_events events;
	std::unique_ptr<ds::task_system> tasks;
	std::unique_ptr<edt::job_service> jobs;
	std::vector<edt::connection> connections;
	std::vector<std::string> log;
	ds::lifetime owner{ ds::lifetime_option::cancel_on_destroy };
};

}

BOOST_FIXTURE_TEST_SUITE(EdtJobEventsTests, job_events_fixture)

BOOST_AUTO_TEST_CASE(Events_AreStartedThenProgressThenFinished)
{
	const auto id = start("import", [](ds::task_context& ctx) {
		ctx.set_progress(0.5f, "half");
		return 1;
	});
	BOOST_TEST((log == std::vector<std::string>{ std::format("start:{}:import", id) }));

	run_lane();
	BOOST_TEST(log.size() == 1u); // nothing is reported from the worker; the frame polls
	jobs->pump();
	BOOST_TEST(log.size() == 2u);
	jobs->pump();
	BOOST_TEST(log.size() == 2u); // unchanged: no second event

	deliver();
	const std::vector<std::string> expected{
		std::format("start:{}:import", id),
		std::format("progress:{}:0.5:half", id),
		std::format("finish:{}:done:", id) };
	BOOST_TEST(log == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Progress_IsOneEventPerJobPerPump_InIdOrder)
{
	const auto a = start("a", [](ds::task_context& ctx) { ctx.set_progress(0.25f, "a"); return 1; });
	const auto b = start("b", [](ds::task_context& ctx) { ctx.set_progress(0.75f, "b"); return 1; }, ds::lane::io);
	log.clear();
	run_lane();
	run_lane(ds::lane::io);

	jobs->pump();
	const std::vector<std::string> expected{ std::format("progress:{}:0.25:a", a), std::format("progress:{}:0.75:b", b) };
	BOOST_TEST(log == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Progress_MadeSinceTheLastPump_IsReportedBeforeTheJobFinishes)
{
	const auto id = start("export", [](ds::task_context& ctx) { ctx.set_progress(1.f, "all"); return 1; });
	run_lane();
	deliver(); // no pump in between
	const std::vector<std::string> expected{
		std::format("start:{}:export", id),
		std::format("progress:{}:1:all", id),
		std::format("finish:{}:done:", id) };
	BOOST_TEST(log == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Cancel_BeforeStart_FinishesAsCancelledWithoutProgress)
{
	bool ran = false;
	const auto id = start("work", [&ran](ds::task_context&) { ran = true; return 1; });
	BOOST_TEST(jobs->cancel(id));
	run_lane();
	jobs->pump();
	deliver();

	BOOST_TEST(!ran);
	const std::vector<std::string> expected{ std::format("start:{}:work", id), std::format("finish:{}:cancelled:", id) };
	BOOST_TEST(log == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Cancel_DuringRun_FinishesAsCancelledExactlyOnce)
{
	edt::job_service::job_id id = 0;
	id = start("import", [this, &id](ds::task_context& ctx) {
		jobs->cancel(id);
		ctx.throw_if_stopped();
		return 1;
	});
	run_lane();
	deliver();
	deliver();

	const std::vector<std::string> expected{ std::format("start:{}:import", id), std::format("finish:{}:cancelled:", id) };
	BOOST_TEST(log == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Cancel_AfterTheBodyFinished_StillEndsAsCancelled)
{
	const auto id = start("late", [](ds::task_context&) { return 1; });
	run_lane();
	BOOST_TEST(jobs->cancel(id));
	deliver();
	const std::vector<std::string> expected{ std::format("start:{}:late", id), std::format("finish:{}:cancelled:", id) };
	BOOST_TEST(log == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(FailedJob_FinishesWithItsError)
{
	const auto id = start("work", [](ds::task_context&) -> int { throw std::runtime_error("boom"); });
	run_lane();
	deliver();
	const std::vector<std::string> expected{ std::format("start:{}:work", id), std::format("finish:{}:failed:boom", id) };
	BOOST_TEST(log == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(OwnerGone_TheJobStillFinishesOnce)
{
	edt::job_service::job_id id = 0;
	{
		ds::lifetime short_lived;
		id = jobs->start(ds::lane::compute, { "orphan", "test" }, short_lived,
			[](ds::task_context&) { return 1; }, [](ds::task_result<int>&) {});
		run_lane();
	}
	deliver();
	const std::vector<std::string> expected{ std::format("start:{}:orphan", id), std::format("finish:{}:done:", id) };
	BOOST_TEST(log == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(OnDone_SeesTheJobAsFinished_AndMayStartAnother)
{
	edt::job_service::job_id first = 0;
	edt::job_service::job_id second = 0;
	bool running_inside = true;
	first = jobs->start(ds::lane::compute, { "first", "test" }, owner,
		[](ds::task_context&) { return 1; },
		[&](ds::task_result<int>&) {
			running_inside = jobs->is_running(first);
			second = jobs->start(ds::lane::compute, { "second", "test" }, owner,
				[](ds::task_context&) { return 2; }, [](ds::task_result<int>&) {});
		});
	run_lane();
	deliver();
	BOOST_TEST(!running_inside);
	BOOST_TEST(jobs->is_running(second));
	run_lane();
	deliver();
	BOOST_TEST(log.back() == std::format("finish:{}:done:", second));
}

BOOST_AUTO_TEST_CASE(CancelAllAndWait_FinishesEveryJobAsCancelledInIdOrder)
{
	const auto a = start("a", [](ds::task_context&) { return 1; });
	const auto b = start("b", [](ds::task_context&) { return 1; }, ds::lane::io);
	jobs->cancel_all_and_wait();
	BOOST_TEST(jobs->running_count() == 0u);

	const std::vector<std::string> expected{
		std::format("start:{}:a", a), std::format("start:{}:b", b),
		std::format("finish:{}:cancelled:", a), std::format("finish:{}:cancelled:", b) };
	BOOST_TEST(log == expected, boost::test_tools::per_element());
	run_lane();
	run_lane(ds::lane::io);
	deliver();
	BOOST_TEST(log.size() == expected.size()); // nothing more arrives afterwards
}

BOOST_AUTO_TEST_CASE(List_ShowsRunningJobsOldestFirst_AndInfoKeepsTheOutcome)
{
	const auto a = start("a", [](ds::task_context& ctx) { ctx.set_progress(0.5f, "half"); return 1; });
	const auto b = start("b", [](ds::task_context&) { return 1; });

	auto running = jobs->list();
	BOOST_REQUIRE(running.size() == 2u);
	BOOST_TEST(running[0].id == a);
	BOOST_TEST(running[1].id == b);
	BOOST_TEST(running[0].label == "a");
	BOOST_TEST((running[0].status == edt::job_status::running));

	jobs->cancel(b);
	BOOST_TEST(jobs->info(b)->cancel_requested);
	run_lane();
	deliver();

	BOOST_TEST(jobs->list().empty());
	const auto done = jobs->info(a);
	BOOST_REQUIRE(done.has_value());
	BOOST_TEST((done->status == edt::job_status::done));
	BOOST_TEST(done->fraction == 0.5f);
	BOOST_TEST(done->note == "half");
	BOOST_TEST((jobs->info(b)->status == edt::job_status::cancelled));
	BOOST_TEST(!jobs->info(9999).has_value());
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

struct job_command_fixture : job_events_fixture
{
	job_command_fixture()
		: registrations(edt::register_job_commands(commands, *jobs))
	{
	}

	cmd::result call(const std::string& id, const std::string& args = "{}")
	{
		return commands.invoke(id, json::parse(args), cmd::origin::test());
	}

	cmd::command_registry commands;
	std::vector<edt::registration> registrations;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtJobCommandsTests, job_command_fixture)

BOOST_AUTO_TEST_CASE(Registered_JobListGetAndCancel)
{
	BOOST_TEST(registrations.size() == 3u);
	BOOST_TEST(commands.contains("job.list"));
	BOOST_TEST(commands.contains("job.get"));
	BOOST_TEST(commands.contains("job.cancel"));
}

BOOST_AUTO_TEST_CASE(JobList_ShowsRunningJobs)
{
	BOOST_TEST(call("job.list").value.as_object().at("jobs").as_array().empty());

	const auto id = start("import", [](ds::task_context& ctx) { ctx.set_progress(0.5f, "half"); return 1; });
	const cmd::result listed = call("job.list");
	BOOST_REQUIRE(listed.ok);
	const json::array& jobs_json = listed.value.as_object().at("jobs").as_array();
	BOOST_REQUIRE(jobs_json.size() == 1u);
	const json::object& job = jobs_json[0].as_object();
	BOOST_TEST(job.at("id").as_int64() == static_cast<std::int64_t>(id));
	BOOST_TEST(job.at("label").as_string().c_str() == std::string("import"));
	BOOST_TEST(job.at("status").as_string().c_str() == std::string("running"));
	BOOST_TEST(!job.at("cancel_requested").as_bool());
}

BOOST_AUTO_TEST_CASE(JobGet_ReportsProgressWhileRunningAndTheOutcomeAfterwards)
{
	const auto id = start("export", [](ds::task_context& ctx) { ctx.set_progress(0.5f, "half"); return 1; });
	run_lane();
	const std::string args = std::format(R"({{"id":{}}})", id);

	cmd::result running = call("job.get", args);
	BOOST_REQUIRE_MESSAGE(running.ok, running.error);
	BOOST_TEST(running.value.as_object().at("status").as_string().c_str() == std::string("running"));
	BOOST_TEST(running.value.as_object().at("fraction").as_double() == 0.5);
	BOOST_TEST(running.value.as_object().at("note").as_string().c_str() == std::string("half"));

	deliver();
	cmd::result finished = call("job.get", args);
	BOOST_REQUIRE_MESSAGE(finished.ok, finished.error);
	BOOST_TEST(finished.value.as_object().at("status").as_string().c_str() == std::string("done"));
}

BOOST_AUTO_TEST_CASE(JobGet_OfAFailedJob_CarriesTheError)
{
	const auto id = start("work", [](ds::task_context&) -> int { throw std::runtime_error("boom"); });
	run_lane();
	deliver();
	const cmd::result failed = call("job.get", std::format(R"({{"id":{}}})", id));
	BOOST_REQUIRE_MESSAGE(failed.ok, failed.error);
	BOOST_TEST(failed.value.as_object().at("status").as_string().c_str() == std::string("failed"));
	BOOST_TEST(failed.value.as_object().at("error").as_string().c_str() == std::string("boom"));
}

BOOST_AUTO_TEST_CASE(JobCancel_StopsARunningJob_AndRefusesUnknownAndFinishedOnes)
{
	bool ran = false;
	const auto id = start("work", [&ran](ds::task_context&) { ran = true; return 1; });
	const std::string args = std::format(R"({{"id":{}}})", id);

	const cmd::result cancelled = call("job.cancel", args);
	BOOST_REQUIRE_MESSAGE(cancelled.ok, cancelled.error);
	BOOST_TEST(cancelled.value.as_object().at("cancelled").as_bool());
	BOOST_TEST(call("job.get", args).value.as_object().at("cancel_requested").as_bool());
	run_lane();
	deliver();
	BOOST_TEST(!ran);
	BOOST_TEST(call("job.get", args).value.as_object().at("status").as_string().c_str() == std::string("cancelled"));

	BOOST_TEST(call("job.cancel", args).code == "failed"); // already finished
	BOOST_TEST(call("job.cancel", R"({"id":424242})").code == "not_found");
	BOOST_TEST(call("job.get", R"({"id":424242})").code == "not_found");
}

BOOST_AUTO_TEST_CASE(JobCommands_RejectBadArguments)
{
	BOOST_TEST(call("job.get").code == "invalid_args");
	BOOST_TEST(call("job.get", R"({"id":-1})").code == "invalid_args");
	BOOST_TEST(call("job.cancel", R"({"id":"x"})").code == "invalid_args");
	BOOST_TEST(call("job.list", R"({"extra":1})").code == "invalid_args");
}

BOOST_AUTO_TEST_SUITE_END()
