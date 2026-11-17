#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include <snk/application/app_frame_stage.h>
#include <snk/application/app_loop_service.h>
#include <snk/application/application.h>
#include <snk/command/cmd_main_thread_queue.h>
#include <snk/command/cmd_registry.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

// A game without the editor registers commands in a cmd::command_registry and takes calls from other threads
// through main_thread_queue, drained by the `tasks` frame stage of the engine loop. Nothing here includes an
// editor header: the commands module needs only snk::common and Boost.JSON.

namespace {

boost::json::value json_of(std::string_view text)
{
	return boost::json::parse(text);
}

struct game_fixture
{
	game_fixture()
		: tasks(storage.construct<ds::task_system>(test_util::manual_tasks()))
		, commands(storage.construct<cmd::command_registry>())
		, stages(storage.construct<app::frame_stage_registry>())
	{
		app::register_core_frame_stages(stages);
		// The same stage core_module registers: the one place the main queue is drained.
		stages.add_stage("tasks", app::builtin_stage::input, app::stage_side::after, [](app::stage_context& ctx) {
			ctx.storage.require<ds::task_system>().drain_main();
		});
		storage.construct<app::app_loop_service_interface, app::loop_service>(stages);
		queue = std::make_unique<cmd::main_thread_queue>(commands, tasks);
	}

	void run_frames(int frames)
	{
		int done = 0;
		stages.add_stage("test_frame_counter", app::builtin_stage::present, app::stage_side::after,
			[&done, frames](app::stage_context& c) {
				if (++done >= frames) {
					c.storage.require<app::app_loop_service_interface>().request_stop();
				}
			});
		app::application application;
		BOOST_TEST(application.run(storage) == 0);
		stages.remove_stage("test_frame_counter");
	}

	ds::app_data_storage storage;
	ds::task_system& tasks;
	cmd::command_registry& commands;
	app::frame_stage_registry& stages;
	std::unique_ptr<cmd::main_thread_queue> queue; // last: destroyed before the registry and the task system
};

}

BOOST_FIXTURE_TEST_SUITE(CmdEngineWithoutEditorTests, game_fixture)

BOOST_AUTO_TEST_CASE(GameRegistersAndInvokesItsCommands)
{
	int score = 0;
	cmd::command_desc desc;
	desc.id = "game.add_score";
	desc.summary = "Adds points";
	desc.args_schema = json_of(R"({"type":"object","properties":{"points":{"type":"integer","minimum":1}},"required":["points"],"additionalProperties":false})");
	desc.result_schema = json_of(R"({"type":"object","properties":{"score":{"type":"integer"}},"required":["score"]})");
	desc.mutates = true;
	desc.invoke = [&score](const cmd::call& request) {
		score += static_cast<int>(request.args.as_object().at("points").as_int64());
		return cmd::result::success(boost::json::object{ { "score", score } });
	};
	auto reg = commands.add(std::move(desc));
	BOOST_REQUIRE(reg.active());

	const auto outcome = commands.invoke("game.add_score", json_of(R"({"points":5})"), cmd::origin::game());
	BOOST_CHECK(outcome.ok);
	BOOST_CHECK_EQUAL(boost::json::serialize(outcome.value), R"({"score":5})");
	BOOST_CHECK_EQUAL(commands.invoke("game.add_score", json_of(R"({"points":0})")).code, "invalid_args");
	BOOST_CHECK_EQUAL(score, 5);
}

BOOST_AUTO_TEST_CASE(CallFromAnotherThread_IsAnsweredByTheTasksStageOfTheFrameLoop)
{
	std::thread::id ran_on;
	cmd::command_desc desc;
	desc.id = "game.ping";
	desc.result_schema = json_of(R"({"type":"object","properties":{"pong":{"type":"boolean"}},"required":["pong"]})");
	desc.invoke = [&ran_on](const cmd::call&) {
		ran_on = std::this_thread::get_id();
		return cmd::result::success(boost::json::object{ { "pong", true } });
	};
	auto reg = commands.add(std::move(desc));

	std::future<cmd::result> outcome;
	std::thread client([&] { outcome = queue->post("game.ping", boost::json::object{}, cmd::origin::agent("client-1")); });
	client.join();
	BOOST_CHECK(outcome.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

	run_frames(1);

	BOOST_REQUIRE(outcome.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
	const cmd::result result = outcome.get();
	BOOST_CHECK(result.ok);
	BOOST_CHECK_EQUAL(boost::json::serialize(result.value), R"({"pong":true})");
	BOOST_CHECK(ran_on == std::this_thread::get_id());
}

BOOST_AUTO_TEST_CASE(CallForACommandRemovedBeforeTheFrame_IsNotFound)
{
	cmd::command_desc desc;
	desc.id = "game.temporary";
	desc.invoke = [](const cmd::call&) { return cmd::result::success(); };
	auto reg = commands.add(std::move(desc));

	auto outcome = queue->post("game.temporary", boost::json::object{}, cmd::origin::test());
	reg.reset();
	run_frames(1);
	BOOST_CHECK_EQUAL(outcome.get().code, "not_found");
}

BOOST_AUTO_TEST_SUITE_END()
