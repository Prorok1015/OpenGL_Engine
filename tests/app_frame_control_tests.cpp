#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include <snk/application/app_frame_commands.h>
#include <snk/application/app_frame_control.h>
#include <snk/application/app_frame_stage.h>
#include <snk/application/app_loop_service.h>
#include <snk/command/cmd_main_thread_queue.h>
#include <snk/command/cmd_registry.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <vector>

// app::frame_control and the `app.advance` command. Time is made by hand (deterministic_time with a fixed dt),
// so every dt a stage sees is known and nothing here sleeps.

namespace {

namespace json = boost::json;

// A loop with a stage that records the dt of each frame. `before_update` runs at the `input` stage of the frame
// with its number (1-based), the way a command does in the `tasks` stage.
struct loop_fixture
{
	explicit loop_fixture(app::deterministic_time time)
		: control(time)
		, loop(stages, time, &control)
	{
		stages.set_body(app::builtin_stage::update, [this](app::stage_context& c) { dts.push_back(c.dt.count()); });
		stages.add_stage("test_commands", app::builtin_stage::input, app::stage_side::after, [this](app::stage_context&) {
			if (on_frame) on_frame(control.frame());
		});
		loop.init(storage);
	}

	void step(int frames)
	{
		for (int i = 0; i < frames; ++i) loop.on_step(storage);
	}

	app::frame_stage_registry stages;
	app::frame_control control;
	app::loop_service loop;
	ds::app_data_storage storage;
	std::vector<float> dts;
	std::function<void(std::uint64_t)> on_frame;
};

}

BOOST_AUTO_TEST_SUITE(AppFrameControlTests)

BOOST_AUTO_TEST_CASE(NeverAdvanced_PassesTheLoopTimeThrough)
{
	app::frame_control control;
	BOOST_CHECK(!control.holding());
	BOOST_CHECK_EQUAL(control.take_dt(0.3f), 0.3f);
	BOOST_CHECK_EQUAL(control.take_dt(0.f), 0.f);
}

BOOST_AUTO_TEST_CASE(Advance_GrantsFramesAtTheFixedDtThenHoldsTheTime)
{
	app::frame_control control(app::deterministic_time{ 0.25f, 0 });
	control.advance(2);

	BOOST_CHECK(control.holding());
	BOOST_CHECK_EQUAL(control.pending(), 2u);
	BOOST_CHECK_EQUAL(control.take_dt(9.f), 0.25f);
	BOOST_CHECK_EQUAL(control.take_dt(9.f), 0.25f);
	BOOST_CHECK_EQUAL(control.pending(), 0u);
	BOOST_CHECK_EQUAL(control.take_dt(9.f), 0.f);
	BOOST_CHECK_EQUAL(control.take_dt(9.f), 0.f);
}

BOOST_AUTO_TEST_CASE(Advance_AddsUpAndTakesANewFixedDt)
{
	app::frame_control control;
	BOOST_CHECK_EQUAL(control.fixed_dt(), 1.f / 60.f);
	control.advance(1);
	control.advance(2, 0.5f);

	BOOST_CHECK_EQUAL(control.pending(), 3u);
	BOOST_CHECK_EQUAL(control.fixed_dt(), 0.5f);
	control.advance(0, -1.f);
	BOOST_CHECK_EQUAL(control.fixed_dt(), 0.5f);
}

BOOST_AUTO_TEST_CASE(FixedDt_StartsFromTheDeterministicTime)
{
	BOOST_CHECK_EQUAL(app::frame_control(app::deterministic_time{ 0.125f, 0 }).fixed_dt(), 0.125f);
	BOOST_CHECK_EQUAL(app::frame_control(app::deterministic_time{}).fixed_dt(), 1.f / 60.f);
}

BOOST_AUTO_TEST_CASE(Loop_CountsFramesAndLeavesTheTimeAloneUntilAdvanced)
{
	loop_fixture fixture(app::deterministic_time{ 0.5f, 0 });

	fixture.step(3);

	BOOST_CHECK_EQUAL(fixture.control.frame(), 3u);
	BOOST_REQUIRE_EQUAL(fixture.dts.size(), 3u);
	for (const float dt : fixture.dts) {
		BOOST_CHECK_EQUAL(dt, 0.5f);
	}
}

BOOST_AUTO_TEST_CASE(Loop_RunsOnlyTheGrantedFramesAfterAnAdvance)
{
	loop_fixture fixture(app::deterministic_time{ 0.5f, 0 });
	fixture.on_frame = [&fixture](std::uint64_t frame) {
		if (frame == 1) fixture.control.advance(2, 0.1f);
	};

	fixture.step(5);

	// Frame 1 had its time before the command ran; the two granted frames follow; then the time is held.
	const std::vector<float> expected{ 0.5f, 0.1f, 0.1f, 0.f, 0.f };
	BOOST_CHECK_EQUAL_COLLECTIONS(fixture.dts.begin(), fixture.dts.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(Loop_AdvanceThawsATimeFrozenByFreezeAfterFrames)
{
	loop_fixture fixture(app::deterministic_time{ 0.5f, 1 });
	fixture.on_frame = [&fixture](std::uint64_t frame) {
		if (frame == 3) fixture.control.advance(1);
	};

	fixture.step(5);

	// freeze_after_frames = 1: frame 1 runs, every later frame has dt 0 until app.advance grants frame 4.
	const std::vector<float> expected{ 0.5f, 0.f, 0.f, 0.5f, 0.f };
	BOOST_CHECK_EQUAL_COLLECTIONS(fixture.dts.begin(), fixture.dts.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(Loop_RecordsTheFrameTheSceneSettledAt)
{
	loop_fixture fixture(app::deterministic_time{ 0.5f, 1 });
	BOOST_CHECK(!fixture.control.settled_frame().has_value());

	fixture.step(4);

	BOOST_REQUIRE(fixture.control.settled_frame().has_value());
	BOOST_CHECK_EQUAL(*fixture.control.settled_frame(), 2u);
}

BOOST_AUTO_TEST_CASE(Loop_RunsOnItsOwnTimeAgainAfterRelease)
{
	loop_fixture fixture(app::deterministic_time{ 0.5f, 0 });
	fixture.on_frame = [&fixture](std::uint64_t frame) {
		if (frame == 1) fixture.control.advance(1, 0.1f);
		if (frame == 3) fixture.control.release();
	};

	fixture.step(5);

	const std::vector<float> expected{ 0.5f, 0.1f, 0.f, 0.5f, 0.5f };
	BOOST_CHECK_EQUAL_COLLECTIONS(fixture.dts.begin(), fixture.dts.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(Loop_StartingAgainCountsFromZero)
{
	loop_fixture fixture(app::deterministic_time{ 0.5f, 0 });
	fixture.step(2);
	fixture.loop.init(fixture.storage);
	BOOST_CHECK_EQUAL(fixture.control.frame(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

struct command_fixture
{
	app::frame_control control;
	cmd::command_registry commands;
	app::frame_command_registrations registration = app::register_frame_commands(commands, control);

	cmd::result advance(const std::string& args)
	{
		return commands.invoke("app.advance", json::parse(args), cmd::origin::test());
	}
};

}

BOOST_FIXTURE_TEST_SUITE(AppAdvanceCommandTests, command_fixture)

BOOST_AUTO_TEST_CASE(Registered_WithStrictSchemas)
{
	BOOST_REQUIRE(registration.active());
	const cmd::command_desc* desc = commands.find("app.advance");
	BOOST_REQUIRE(desc != nullptr);
	BOOST_CHECK(desc->expose_as_tool);
	// Holds the time of the process until app.resume: not read-only, so the read-only MCP server does not list it.
	BOOST_CHECK(desc->mutates);
	BOOST_CHECK(desc->risk == cmd::danger::none);
	BOOST_CHECK(commands.contains("app.resume"));
	BOOST_REQUIRE(commands.find("app.resume") != nullptr);
	BOOST_CHECK(commands.find("app.resume")->mutates);
	BOOST_CHECK(!desc->args_schema.as_object().at("additionalProperties").as_bool());
	BOOST_CHECK(!desc->result_schema.as_object().at("additionalProperties").as_bool());
}

BOOST_AUTO_TEST_CASE(Resume_ReleasesTheHoldAndDropsPendingFrames)
{
	BOOST_REQUIRE(registration.resume.active());
	BOOST_REQUIRE(advance(R"({"frames":5})").ok);
	BOOST_REQUIRE(control.holding());

	const auto result = commands.invoke("app.resume", json::parse("{}"), cmd::origin::test());

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_CHECK(!control.holding());
	BOOST_CHECK_EQUAL(control.pending(), 0u);
	BOOST_CHECK_EQUAL(control.take_dt(0.3f), 0.3f);
	BOOST_CHECK_EQUAL(commands.invoke("app.resume", json::parse(R"({"x":1})"), cmd::origin::test()).code, cmd::codes::invalid_args);
	// Resuming without a hold is harmless, and a new advance holds again.
	BOOST_CHECK(commands.invoke("app.resume", json::parse("{}"), cmd::origin::test()).ok);
	BOOST_CHECK(advance(R"({"frames":1})").ok);
	BOOST_CHECK(control.holding());
}

BOOST_AUTO_TEST_CASE(Advance_GrantsFramesAndAnswersWithTheTarget)
{
	control.begin_frame();
	control.begin_frame();

	const auto result = advance(R"({"frames":3})");

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_CHECK_EQUAL(result.value.as_object().at("frame").as_int64(), 2);
	BOOST_CHECK_EQUAL(result.value.as_object().at("target_frame").as_int64(), 5);
	BOOST_CHECK_EQUAL(control.pending(), 3u);

	const auto more = advance(R"({"frames":2})");
	BOOST_CHECK_EQUAL(more.value.as_object().at("target_frame").as_int64(), 7);
}

BOOST_AUTO_TEST_CASE(Advance_TakesTheFixedDtAndIntegersWrittenAsDoubles)
{
	const auto result = advance(R"({"frames":4.0,"fixed_dt":0.1})");

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_CHECK_EQUAL(control.pending(), 4u);
	BOOST_CHECK_EQUAL(control.fixed_dt(), static_cast<float>(0.1));
}

BOOST_AUTO_TEST_CASE(Advance_RefusesBadArguments)
{
	for (const char* bad : { R"({})", R"({"frames":0})", R"({"frames":-1})", R"({"frames":1.5})", R"({"frames":1000001})",
		R"({"frames":1,"fixed_dt":0})", R"({"frames":1,"fixed_dt":-0.1})", R"({"frames":1,"fixed_dt":2})",
		R"({"frames":1,"extra":true})", R"({"frames":"3"})" }) {
		const auto result = advance(bad);
		BOOST_CHECK_MESSAGE(!result.ok && result.code == cmd::codes::invalid_args, bad);
	}
	BOOST_CHECK(!control.holding());
}

BOOST_AUTO_TEST_CASE(Advance_PostedThroughTheMainQueueRunsInTheTasksStage)
{
	ds::task_system tasks{ test_util::manual_tasks() };
	cmd::main_thread_queue queue{ commands, tasks };
	app::frame_stage_registry stages;
	std::vector<float> dts;
	stages.add_stage("tasks", app::builtin_stage::input, app::stage_side::after, [&tasks](app::stage_context&) {
		tasks.drain_main(std::chrono::hours(1));
	});
	stages.set_body(app::builtin_stage::update, [&dts](app::stage_context& c) { dts.push_back(c.dt.count()); });
	const app::deterministic_time time{ 0.5f, 0 };
	app::loop_service loop(stages, time, &control);
	ds::app_data_storage storage;
	loop.init(storage);

	auto outcome = queue.post("app.advance", json::parse(R"({"frames":2,"fixed_dt":0.25})"), cmd::origin::agent("session-1"));
	loop.on_step(storage);
	loop.on_step(storage);
	loop.on_step(storage);
	loop.on_step(storage);

	BOOST_REQUIRE(outcome.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
	BOOST_CHECK(outcome.get().ok);
	const std::vector<float> expected{ 0.5f, 0.25f, 0.25f, 0.f };
	BOOST_CHECK_EQUAL_COLLECTIONS(dts.begin(), dts.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_SUITE_END()
