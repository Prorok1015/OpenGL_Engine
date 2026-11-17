#include <boost/test/unit_test.hpp>
#include <chrono>
#include <optional>
#include <string>
#include <vector>
#include <snk/application/app_frame_stage.h>
#include <snk/application/app_loop_service.h>
#include <snk/application/application.h>

BOOST_AUTO_TEST_SUITE(AppLoopServiceTests)

BOOST_AUTO_TEST_CASE(StepRunsRegistryStagesInOrder) {
	app::frame_stage_registry stages;
	std::string trace;
	const auto mark = [&trace](char c) {
		return [&trace, c](app::stage_context&) { trace += c; };
	};
	stages.set_body(app::builtin_stage::update, mark('U'));
	stages.add_stage("before_update", app::builtin_stage::update, app::stage_side::before, mark('b'));
	stages.add_stage("after_update", app::builtin_stage::update, app::stage_side::after, mark('a'));

	ds::app_data_storage storage;
	app::loop_service loop(stages);
	loop.init(storage);
	loop.on_step(storage);
	loop.on_step(storage);
	BOOST_TEST(trace == "bUabUa");
}

BOOST_AUTO_TEST_CASE(StageSeesStorageAndPositiveDelta) {
	struct counter { int value = 0; };
	app::frame_stage_registry stages;
	float seen_dt = -1.f;
	stages.set_body(app::builtin_stage::update, [&](app::stage_context& c) {
		c.storage.require<counter>().value++;
		seen_dt = c.dt.count();
	});

	ds::app_data_storage storage;
	storage.construct<counter>();
	app::loop_service loop(stages);
	loop.init(storage);
	loop.on_step(storage);
	BOOST_TEST(storage.require<counter>().value == 1);
	BOOST_TEST(seen_dt >= 0.f);
}

BOOST_AUTO_TEST_CASE(RequestStopEndsLoop) {
	app::frame_stage_registry stages;
	ds::app_data_storage storage;
	app::loop_service loop(stages);
	loop.init(storage);
	BOOST_TEST(!loop.should_stop());
	loop.request_stop();
	BOOST_TEST(loop.should_stop());
}

BOOST_AUTO_TEST_CASE(InitAndTermHooksRunInOrder) {
	app::frame_stage_registry stages;
	std::string trace;
	stages.add_init_hook([&](ds::app_data_storage&) { trace += "i1"; });
	stages.add_init_hook([&](ds::app_data_storage&) { trace += "i2"; });
	stages.add_term_hook([&](ds::app_data_storage&) { trace += "t1"; });
	stages.add_term_hook([&](ds::app_data_storage&) { trace += "t2"; });

	ds::app_data_storage storage;
	app::loop_service loop(stages);
	loop.init(storage);
	loop.term(storage);
	BOOST_TEST(trace == "i1i2t2t1");
}

BOOST_AUTO_TEST_CASE(RemovedStageAndClearedBodyNoLongerRun) {
	app::frame_stage_registry stages;
	std::string trace;
	const auto mark = [&trace](char c) {
		return [&trace, c](app::stage_context&) { trace += c; };
	};
	stages.set_body(app::builtin_stage::ui, mark('u'));
	stages.add_stage("extra", app::builtin_stage::ui, app::stage_side::after, mark('x'));
	BOOST_TEST(!stages.remove_stage("ui"));
	BOOST_TEST(stages.remove_stage("extra"));
	BOOST_TEST(!stages.remove_stage("extra"));
	stages.clear_body(app::builtin_stage::ui);

	ds::app_data_storage storage;
	app::stage_context ctx{ storage, std::chrono::duration<float>{ 0.f } };
	stages.run(ctx);
	BOOST_TEST(trace.empty());
	BOOST_TEST(stages.set_body(app::builtin_stage::ui, mark('u')));
}

BOOST_AUTO_TEST_CASE(DeterministicTimeFeedsFixedDtThenFreezes) {
	app::frame_stage_registry stages;
	std::vector<float> dts;
	stages.set_body(app::builtin_stage::update, [&](app::stage_context& c) { dts.push_back(c.dt.count()); });

	ds::app_data_storage storage;
	app::loop_service loop(stages, app::deterministic_time{ 0.25f, 2 });
	loop.init(storage);
	for (int i = 0; i < 4; ++i) {
		loop.on_step(storage);
	}
	BOOST_TEST(dts == (std::vector<float>{ 0.25f, 0.25f, 0.f, 0.f }));
}

BOOST_AUTO_TEST_CASE(DeterministicTimeCountsFramesFromReady) {
	app::frame_stage_registry stages;
	std::vector<float> dts;
	stages.set_body(app::builtin_stage::update, [&](app::stage_context& c) { dts.push_back(c.dt.count()); });

	ds::app_data_storage storage;
	app::loop_service loop(stages, app::deterministic_time{ 0.25f, 2 });
	int step = 0;
	// Not ready on frames 0-1; the count starts at frame 2, so the dt is frozen from frame 4.
	loop.set_ready_probe([&](ds::app_data_storage&) { return step++ >= 2; });
	loop.init(storage);
	for (int i = 0; i < 7; ++i) {
		loop.on_step(storage);
	}
	BOOST_TEST(dts == (std::vector<float>{ 0.25f, 0.25f, 0.25f, 0.25f, 0.f, 0.f, 0.f }));
}

BOOST_AUTO_TEST_CASE(ExitAfterFramesStopsTheRunWithZero) {
	app::frame_stage_registry stages;
	int frames = 0;
	stages.set_body(app::builtin_stage::update, [&frames](app::stage_context&) { ++frames; });

	ds::app_data_storage storage;
	auto& loop = storage.construct<app::app_loop_service_interface, app::loop_service>(stages);
	static_cast<app::loop_service&>(loop).set_exit_after_frames(5);
	app::application application;
	BOOST_TEST(application.run(storage) == 0);
	BOOST_TEST(frames == 5);
	// A second run counts from the start again.
	frames = 0;
	BOOST_TEST(application.run(storage) == 0);
	BOOST_TEST(frames == 5);
}

BOOST_AUTO_TEST_CASE(ExitAfterZeroFramesNeverStops) {
	app::frame_stage_registry stages;
	ds::app_data_storage storage;
	app::loop_service loop(stages);
	loop.set_exit_after_frames(0);
	loop.init(storage);
	for (int i = 0; i < 10; ++i) loop.on_step(storage);
	BOOST_TEST(!loop.should_stop());
}

BOOST_AUTO_TEST_CASE(FrameDeadline_IsOneFramePeriodAfterTheStart) {
	using namespace std::chrono;
	const steady_clock::time_point start{ seconds(100) };
	const auto at_60 = app::frame_deadline(start, 60);
	BOOST_REQUIRE(at_60.has_value());
	BOOST_TEST(duration_cast<nanoseconds>(*at_60 - start).count() == 16666666);
	const auto at_1 = app::frame_deadline(start, 1);
	BOOST_REQUIRE(at_1.has_value());
	BOOST_TEST((*at_1 - start == seconds(1)));
	BOOST_TEST(!app::frame_deadline(start, 0).has_value());
	BOOST_TEST(!app::frame_deadline(start, -5).has_value());
}

BOOST_AUTO_TEST_CASE(HeadlessFrameLimit_AppliesOnlyWithoutAWindow) {
	BOOST_TEST(app::headless_frame_limit(60, true) == 60);
	BOOST_TEST(app::headless_frame_limit(60, false) == 0);
	BOOST_TEST(app::headless_frame_limit(0, true) == 0);
	BOOST_TEST(app::headless_frame_limit(-1, true) == 0);
}

BOOST_AUTO_TEST_CASE(FrameLimit_SleepsUntilTheDeadlineOfEachFrame) {
	app::frame_stage_registry stages;
	ds::app_data_storage storage;
	app::loop_service loop(stages);
	std::vector<std::chrono::steady_clock::time_point> deadlines;
	loop.set_sleep_until([&deadlines](std::chrono::steady_clock::time_point t) { deadlines.push_back(t); });
	loop.set_frame_limit(50);
	loop.init(storage);
	const auto before = std::chrono::steady_clock::now();
	loop.on_step(storage);
	const auto after = std::chrono::steady_clock::now();
	BOOST_REQUIRE(deadlines.size() == 1u);
	// The frame started between `before` and `after`; the deadline is 20 ms after its start.
	BOOST_TEST((deadlines[0] >= before + std::chrono::milliseconds(20)));
	BOOST_TEST((deadlines[0] <= after + std::chrono::milliseconds(20)));
}

BOOST_AUTO_TEST_CASE(FrameLimit_ZeroOrAStoppingLoop_NeverSleeps) {
	app::frame_stage_registry stages;
	ds::app_data_storage storage;
	app::loop_service loop(stages);
	int sleeps = 0;
	loop.set_sleep_until([&sleeps](std::chrono::steady_clock::time_point) { ++sleeps; });
	loop.init(storage);
	loop.on_step(storage);
	BOOST_TEST(sleeps == 0);

	// The last frame ends at once: the run is over.
	loop.set_frame_limit(30);
	loop.set_exit_after_frames(2);
	loop.on_step(storage);
	BOOST_TEST(sleeps == 0);
	BOOST_TEST(loop.should_stop());
}

BOOST_AUTO_TEST_SUITE_END()
