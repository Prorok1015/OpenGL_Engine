#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_service_commands.h>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <format>
#include <memory>
#include <optional>
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

BOOST_AUTO_TEST_CASE(Destructor_DiscardsAResultThatWasReadyButNotDelivered)
{
	// The body ran, the result waits for the main drain; the service goes first: nothing may call back into it.
	int delivered = 0;
	jobs->start(ds::lane::compute, { "work", "test" }, owner,
		[](ds::task_context&) { return 1; },
		[&delivered](ds::task_result<int>&) { ++delivered; });
	run_lane();
	jobs.reset();
	deliver();
	BOOST_TEST(delivered == 0);
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

BOOST_AUTO_TEST_CASE(StartWithResult_KeepsTheValueOfADoneJob)
{
	bool done_called = false;
	const auto id = jobs->start_with_result(ds::lane::io, { "status", "test" }, owner,
		[](ds::task_context&) { return boost::json::value(boost::json::object{ { "branch", "main" } }); },
		[&done_called](ds::task_result<boost::json::value>& r) { done_called = r.ok(); });
	BOOST_REQUIRE(jobs->info(id).has_value());
	BOOST_TEST(!jobs->info(id)->result.has_value());

	run_lane(ds::lane::io);
	deliver();
	const auto finished = jobs->info(id);
	BOOST_REQUIRE(finished.has_value());
	BOOST_TEST((finished->status == edt::job_status::done));
	BOOST_REQUIRE(finished->result.has_value());
	BOOST_TEST(std::string(finished->result->as_object().at("branch").as_string()) == "main");
	BOOST_TEST(done_called);
}

BOOST_AUTO_TEST_CASE(StoredResults_StayUnderTheBudget_OldestDroppedFirst)
{
	constexpr std::size_t half = edt::job_service::RESULT_BUDGET / 2;
	const auto first = jobs->start_with_result(ds::lane::io, { "first", "test" }, owner,
		[](ds::task_context&) { return boost::json::value(boost::json::string(std::string(half, 'a'))); });
	run_lane(ds::lane::io);
	deliver();
	BOOST_TEST(jobs->info(first)->result.has_value());

	const auto second = jobs->start_with_result(ds::lane::io, { "second", "test" }, owner,
		[](ds::task_context&) { return boost::json::value(boost::json::string(std::string(half, 'b'))); });
	run_lane(ds::lane::io);
	deliver();
	const auto dropped = jobs->info(first);
	BOOST_REQUIRE(dropped.has_value());
	BOOST_TEST((dropped->status == edt::job_status::done));
	BOOST_TEST(!dropped->result.has_value());
	BOOST_TEST(dropped->result_dropped);
	BOOST_TEST(jobs->info(second)->result.has_value());
	BOOST_TEST(!jobs->info(second)->result_dropped);

	const auto third = jobs->start_with_result(ds::lane::io, { "third", "test" }, owner,
		[](ds::task_context&) { return boost::json::value(1); });
	run_lane(ds::lane::io);
	deliver();
	BOOST_TEST(jobs->info(second)->result.has_value());
	BOOST_TEST(jobs->info(third)->result.has_value());
}

BOOST_AUTO_TEST_CASE(StartWithResult_WithoutOnDone_StillKeepsTheValue)
{
	const auto id = jobs->start_with_result(ds::lane::io, { "log", "test" }, owner,
		[](ds::task_context&) { return boost::json::value(42); });
	run_lane(ds::lane::io);
	deliver();
	const auto finished = jobs->info(id);
	BOOST_REQUIRE(finished.has_value() && finished->result.has_value());
	BOOST_TEST(finished->result->as_int64() == 42);
}

BOOST_AUTO_TEST_CASE(Start_ReturningJson_KeepsNoResult)
{
	const auto id = jobs->start(ds::lane::io, { "plain", "test" }, owner,
		[](ds::task_context&) { return boost::json::value(1); },
		[](ds::task_result<boost::json::value>&) {});
	run_lane(ds::lane::io);
	deliver();
	const auto finished = jobs->info(id);
	BOOST_REQUIRE(finished.has_value());
	BOOST_TEST(!finished->result.has_value());
}

BOOST_AUTO_TEST_CASE(StartWithResult_FailedOrCancelledJobs_HaveNoResult)
{
	const auto failed = jobs->start_with_result(ds::lane::io, { "fails", "test" }, owner,
		[](ds::task_context&) -> boost::json::value { throw std::runtime_error("boom"); });
	const auto cancelled = jobs->start_with_result(ds::lane::io, { "cancelled", "test" }, owner,
		[](ds::task_context&) { return boost::json::value(1); });
	BOOST_TEST(jobs->cancel(cancelled));
	run_lane(ds::lane::io);
	deliver();

	const auto failed_info = jobs->info(failed);
	BOOST_REQUIRE(failed_info.has_value());
	BOOST_TEST((failed_info->status == edt::job_status::failed));
	BOOST_TEST(!failed_info->result.has_value());
	const auto cancelled_info = jobs->info(cancelled);
	BOOST_REQUIRE(cancelled_info.has_value());
	BOOST_TEST((cancelled_info->status == edt::job_status::cancelled));
	BOOST_TEST(!cancelled_info->result.has_value());
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
		connections.push_back(events.job_finished.subscribe([this](std::uint64_t id, edt::job_status status, const std::string& error,
			const std::optional<boost::json::value>&) {
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
		std::format("progress:{}:1:half", id), // the task system completes the fraction when the body returns
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
	const std::vector<std::string> expected{ std::format("progress:{}:1:a", a), std::format("progress:{}:1:b", b) };
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

BOOST_AUTO_TEST_CASE(FailedJob_IsShownAsAnErrorDialogWithItsLabelAndError)
{
	edt::dialog_service dialogs;
	dialogs.set_registrar([](std::string_view, std::function<bool()>) {});
	const edt::connection shown = edt::report_failed_jobs(events, *jobs, dialogs);

	start("Import tri.fbx", [](ds::task_context&) -> int { throw std::runtime_error("boom"); });
	run_lane();
	deliver();

	const auto waiting = dialogs.list();
	BOOST_REQUIRE_EQUAL(waiting.size(), 1u);
	BOOST_TEST(waiting[0].kind == "message");
	BOOST_TEST(waiting[0].text.find("Import tri.fbx") != std::string::npos);
	BOOST_TEST(waiting[0].text.find("boom") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(DoneAndCancelledJobs_ShowNoDialog)
{
	edt::dialog_service dialogs;
	dialogs.set_registrar([](std::string_view, std::function<bool()>) {});
	const edt::connection shown = edt::report_failed_jobs(events, *jobs, dialogs);

	start("fine", [](ds::task_context&) { return 1; });
	const auto cancelled = start("stopped", [](ds::task_context&) { return 1; });
	jobs->cancel(cancelled);
	run_lane();
	deliver();

	BOOST_TEST(dialogs.pending() == 0u);
}

BOOST_AUTO_TEST_CASE(FailedJob_WithoutADialogRegistrar_IsOnlyLogged)
{
	edt::dialog_service dialogs;
	const edt::connection shown = edt::report_failed_jobs(events, *jobs, dialogs);

	start("work", [](ds::task_context&) -> int { throw std::runtime_error("boom"); });
	run_lane();
	deliver();

	BOOST_TEST(dialogs.pending() == 0u);
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
	const std::vector<std::string> expected{ std::format("start:{}:orphan", id), std::format("progress:{}:1:", id), std::format("finish:{}:done:", id) };
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
	BOOST_TEST(done->fraction == 1.f);
	BOOST_TEST(done->note == "half");
	BOOST_TEST((jobs->info(b)->status == edt::job_status::cancelled));
	BOOST_TEST(!jobs->info(9999).has_value());
}

// A slot reacting to a finished job may start another one while the service is being torn down: that one must be
// cancelled too, not wiped from the books while its task keeps running.
BOOST_AUTO_TEST_CASE(CancelAllAndWait_AlsoStopsAJobStartedByAFinishedSlot)
{
	// Shared with the slot and the job body, which the fixture keeps beyond this test body.
	struct follow_state
	{
		bool ran = false;
		edt::job_service::job_id id = 0;
	};
	auto state = std::make_shared<follow_state>();
	connections.push_back(events.job_finished.subscribe([this, state](std::uint64_t, edt::job_status, const std::string&,
		const std::optional<boost::json::value>&) {
		if (state->id == 0) {
			state->id = start("follow_up", [state](ds::task_context&) { state->ran = true; return 1; });
		}
	}));
	start("first", [](ds::task_context&) { return 1; });

	jobs->cancel_all_and_wait();
	BOOST_TEST(jobs->running_count() == 0u);
	BOOST_REQUIRE(state->id != 0);
	BOOST_TEST(!jobs->is_running(state->id));
	BOOST_TEST((jobs->info(state->id)->status == edt::job_status::cancelled));

	run_lane();
	deliver();
	BOOST_TEST(!state->ran);
	BOOST_TEST(tasks->snapshot().empty());
}

BOOST_AUTO_TEST_CASE(JobFinished_CarriesTheResultOfADoneJob)
{
	// Shared with the slot, which the fixture keeps beyond this test body.
	auto results = std::make_shared<std::vector<std::optional<boost::json::value>>>();
	connections.push_back(events.job_finished.subscribe([results](std::uint64_t, edt::job_status, const std::string&,
		const std::optional<boost::json::value>& result) { results->push_back(result); }));
	jobs->start_with_result(ds::lane::io, { "with result", "test" }, owner,
		[](ds::task_context&) { return boost::json::value("value"); });
	start("plain", [](ds::task_context&) { return 1; }, ds::lane::io);

	run_lane(ds::lane::io);
	deliver();
	BOOST_REQUIRE(results->size() == 2u);
	const auto with_value = std::ranges::find_if(*results, [](const auto& r) { return r.has_value(); });
	BOOST_REQUIRE(with_value != results->end());
	BOOST_TEST(std::string((*with_value)->as_string()) == "value");
	BOOST_TEST(std::ranges::count_if(*results, [](const auto& r) { return r.has_value(); }) == 1);
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
	for (const char* id : { "job.list", "job.get" }) {
		BOOST_TEST_CONTEXT(id) {
			BOOST_TEST((commands.find(id)->category == cmd::agent_category::read));
		}
	}
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
	BOOST_TEST(running.value.as_object().at("fraction").as_double() == 1.0);
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

// Python's json writes 3.0 for an integer; the schema accepts it, so the commands must read it instead of throwing.
BOOST_AUTO_TEST_CASE(JobCommands_AcceptIntegralDoublesForTheId)
{
	const auto id = start("work", [](ds::task_context&) { return 1; });
	const std::string args = std::format(R"({{"id":{}.0}})", id);

	cmd::result got;
	BOOST_CHECK_NO_THROW(got = call("job.get", args));
	BOOST_REQUIRE_MESSAGE(got.ok, got.error);
	BOOST_TEST(got.value.as_object().at("id").as_int64() == static_cast<std::int64_t>(id));
	BOOST_CHECK_NO_THROW(got = call("job.cancel", args));
	BOOST_TEST(got.ok);
	BOOST_TEST(call("job.get", R"({"id":1e30})").code == "invalid_args");
	BOOST_TEST(call("job.get", R"({"id":2.5})").code == "invalid_args");
}

BOOST_AUTO_TEST_CASE(JobCommands_OnlyCancelChangesAnything)
{
	for (const char* id : { "job.list", "job.get", "job.cancel" }) {
		const cmd::command_desc* desc = commands.find(id);
		BOOST_REQUIRE(desc != nullptr);
		BOOST_TEST(desc->mutates == (std::string(id) == "job.cancel"), id);
		BOOST_TEST((desc->risk == cmd::danger::none), id);
	}
}

BOOST_AUTO_TEST_CASE(JobGet_OfADoneJobWithAResult_ReturnsIt)
{
	const auto id = jobs->start_with_result(ds::lane::io, { "vcs.status", "test" }, owner,
		[](ds::task_context&) { return boost::json::value(boost::json::object{ { "clean", true } }); });
	const std::string args = std::format(R"({{"id":{}}})", id);
	BOOST_TEST(!call("job.get", args).value.as_object().contains("result"));

	run_lane(ds::lane::io);
	deliver();
	const cmd::result got = call("job.get", args);
	BOOST_REQUIRE_MESSAGE(got.ok, got.error);
	const json::object& job = got.value.as_object();
	BOOST_REQUIRE(job.contains("result"));
	BOOST_TEST(job.at("result").as_object().at("clean").as_bool());
}

BOOST_AUTO_TEST_SUITE_END()
