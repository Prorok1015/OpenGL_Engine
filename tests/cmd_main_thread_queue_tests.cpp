#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include <snk/command/cmd_main_thread_queue.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// cmd::main_thread_queue: any thread posts, the registry runs the command when the main drain of
// ds::task_system runs. The task system is manual here, so the test is the frame loop.

namespace {

using namespace std::chrono_literals;

boost::json::value json_of(std::string_view text)
{
	return boost::json::parse(text);
}

bool is_ready(std::future<cmd::result>& outcome)
{
	return outcome.wait_for(0ms) == std::future_status::ready;
}

// Posts from another thread, the way a network thread would.
std::future<cmd::result> post_from_worker(cmd::main_thread_queue& queue, std::string id, boost::json::value args, cmd::origin from)
{
	std::future<cmd::result> outcome;
	std::thread worker([&] { outcome = queue.post(std::move(id), std::move(args), std::move(from)); });
	worker.join();
	return outcome;
}

struct queue_fixture
{
	ds::task_system tasks{ test_util::manual_tasks() };
	cmd::command_registry commands;
	cmd::main_thread_queue queue{ commands, tasks };

	std::size_t drain() { return tasks.drain_main(std::chrono::hours(1)); }
};

}

BOOST_FIXTURE_TEST_SUITE(CmdMainThreadQueueTests, queue_fixture)

BOOST_AUTO_TEST_CASE(PostFromWorker_RunsOnTheMainThreadAfterTheDrain)
{
	std::thread::id ran_on;
	int runs = 0;
	cmd::command_desc desc;
	desc.id = "test.sum";
	desc.args_schema = json_of(R"({"type":"object","properties":{"a":{"type":"integer"},"b":{"type":"integer"}},"required":["a","b"]})");
	desc.result_schema = json_of(R"({"type":"object","properties":{"sum":{"type":"integer"}},"required":["sum"]})");
	desc.invoke = [&](const cmd::call& request) {
		++runs;
		ran_on = std::this_thread::get_id();
		const auto& args = request.args.as_object();
		return cmd::result::success(boost::json::object{ { "sum", args.at("a").as_int64() + args.at("b").as_int64() } });
	};
	auto reg = commands.add(std::move(desc));

	auto outcome = post_from_worker(queue, "test.sum", json_of(R"({"a":2,"b":3})"), cmd::origin::test());
	BOOST_REQUIRE(outcome.valid());
	BOOST_CHECK(!is_ready(outcome));
	BOOST_CHECK_EQUAL(runs, 0);
	BOOST_CHECK_EQUAL(tasks.pending_main(ds::main_class::general), 1u);

	BOOST_CHECK_EQUAL(drain(), 1u);
	BOOST_REQUIRE(is_ready(outcome));
	const cmd::result result = outcome.get();
	BOOST_CHECK(result.ok);
	BOOST_CHECK_EQUAL(boost::json::serialize(result.value), R"({"sum":5})");
	BOOST_CHECK_EQUAL(runs, 1);
	BOOST_CHECK(ran_on == std::this_thread::get_id());
}

BOOST_AUTO_TEST_CASE(Post_PassesTheOriginAndGenerationThrough)
{
	cmd::origin seen;
	std::uint64_t generation = 0;
	cmd::command_desc desc;
	desc.id = "test.who";
	desc.invoke = [&](const cmd::call& request) {
		seen = request.from;
		generation = request.expected_generation;
		return cmd::result::success();
	};
	auto reg = commands.add(std::move(desc));

	auto outcome = queue.post("test.who", json_of("{}"), cmd::origin::agent("session-1"), 9);
	drain();
	BOOST_CHECK(outcome.get().ok);
	BOOST_CHECK(seen == cmd::origin::agent("session-1"));
	BOOST_CHECK_EQUAL(generation, 9u);
}

BOOST_AUTO_TEST_CASE(Post_RunsInPostingOrder)
{
	std::vector<int> order;
	cmd::command_desc desc;
	desc.id = "test.push";
	desc.args_schema = json_of(R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})");
	desc.invoke = [&order](const cmd::call& request) {
		order.push_back(static_cast<int>(request.args.as_object().at("n").as_int64()));
		return cmd::result::success();
	};
	auto reg = commands.add(std::move(desc));

	std::vector<std::future<cmd::result>> outcomes;
	for (int n = 0; n < 4; ++n) {
		outcomes.push_back(post_from_worker(queue, "test.push", boost::json::object{ { "n", n } }, cmd::origin::test()));
	}
	BOOST_CHECK_EQUAL(drain(), 4u);
	for (auto& outcome : outcomes) BOOST_CHECK(outcome.get().ok);
	const std::vector<int> expected{ 0, 1, 2, 3 };
	BOOST_TEST(order == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Post_CommandUnregisteredBeforeTheDrain_IsNotFound)
{
	int runs = 0;
	cmd::command_desc desc;
	desc.id = "test.short_lived";
	desc.invoke = [&runs](const cmd::call&) { ++runs; return cmd::result::success(); };
	auto reg = commands.add(std::move(desc));

	auto outcome = post_from_worker(queue, "test.short_lived", json_of("{}"), cmd::origin::test());
	reg.reset();
	BOOST_CHECK(!is_ready(outcome));
	drain();

	BOOST_REQUIRE(is_ready(outcome));
	const cmd::result result = outcome.get();
	BOOST_CHECK(!result.ok);
	BOOST_CHECK_EQUAL(result.code, "not_found");
	BOOST_CHECK_EQUAL(runs, 0);
}

BOOST_AUTO_TEST_CASE(Post_UnknownAndDisabledCallsAnswerThroughTheFuture)
{
	bool on = true;
	int runs = 0;
	cmd::command_desc desc;
	desc.id = "test.gated";
	desc.enabled = [&on] { return on; };
	desc.invoke = [&runs](const cmd::call&) { ++runs; return cmd::result::success(); };
	auto reg = commands.add(std::move(desc));

	auto unknown = queue.post("test.nothing", json_of("{}"), cmd::origin::test());
	auto turned_off = queue.post("test.gated", json_of("{}"), cmd::origin::test());
	on = false; // enabled-ness is read when the call runs, not when it is posted
	drain();

	BOOST_CHECK_EQUAL(unknown.get().code, "not_found");
	BOOST_CHECK_EQUAL(turned_off.get().code, "disabled");
	BOOST_CHECK_EQUAL(runs, 0);
}

BOOST_AUTO_TEST_CASE(Post_ValidatesArgumentsWhenTheCallRuns)
{
	int runs = 0;
	cmd::command_desc desc;
	desc.id = "test.strict";
	desc.args_schema = json_of(R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"],"additionalProperties":false})");
	desc.invoke = [&runs](const cmd::call&) { ++runs; return cmd::result::success(); };
	auto reg = commands.add(std::move(desc));

	auto invalid = queue.post("test.strict", json_of(R"({"n":"x"})"), cmd::origin::test());
	auto not_an_object = queue.post("test.strict", json_of("[1]"), cmd::origin::test());
	auto fine = queue.post("test.strict", json_of(R"({"n":1})"), cmd::origin::test());
	drain();

	BOOST_CHECK_EQUAL(invalid.get().code, "invalid_args");
	BOOST_CHECK_EQUAL(not_an_object.get().code, "invalid_args");
	BOOST_CHECK(fine.get().ok);
	BOOST_CHECK_EQUAL(runs, 1);
}

BOOST_AUTO_TEST_CASE(Post_ACommandMayPostAnotherOneWithoutDeadlock)
{
	int inner_runs = 0;
	std::future<cmd::result> inner;
	cmd::command_desc inner_desc;
	inner_desc.id = "test.inner";
	inner_desc.invoke = [&inner_runs](const cmd::call&) { ++inner_runs; return cmd::result::success(); };
	cmd::command_desc outer_desc;
	outer_desc.id = "test.outer";
	outer_desc.invoke = [&](const cmd::call&) {
		inner = queue.post("test.inner", json_of("{}"), cmd::origin::test());
		return cmd::result::success();
	};
	auto inner_reg = commands.add(std::move(inner_desc));
	auto outer_reg = commands.add(std::move(outer_desc));

	auto outer = queue.post("test.outer", json_of("{}"), cmd::origin::test());
	// Whether the drain that runs the outer call also picks up what it posted is up to the budget; two drains cover both.
	drain();
	BOOST_CHECK(outer.get().ok);
	drain();
	BOOST_REQUIRE(inner.valid());
	BOOST_CHECK(inner.get().ok);
	BOOST_CHECK_EQUAL(inner_runs, 1);
}

BOOST_AUTO_TEST_CASE(Post_ACommandThatThrows_AnswersFailedWithTheExceptionText)
{
	cmd::command_desc std_throw;
	std_throw.id = "test.throws";
	std_throw.invoke = [](const cmd::call&) -> cmd::result { throw std::runtime_error("boom"); };
	cmd::command_desc odd_throw;
	odd_throw.id = "test.throws_odd";
	odd_throw.invoke = [](const cmd::call&) -> cmd::result { throw 42; };
	int later_runs = 0;
	cmd::command_desc fine;
	fine.id = "test.fine";
	fine.invoke = [&later_runs](const cmd::call&) { ++later_runs; return cmd::result::success(); };
	auto r1 = commands.add(std::move(std_throw));
	auto r2 = commands.add(std::move(odd_throw));
	auto r3 = commands.add(std::move(fine));

	auto thrown = queue.post("test.throws", json_of("{}"), cmd::origin::test());
	auto thrown_odd = queue.post("test.throws_odd", json_of("{}"), cmd::origin::test());
	auto after = queue.post("test.fine", json_of("{}"), cmd::origin::test());
	BOOST_CHECK_NO_THROW(drain());

	const cmd::result first = thrown.get();
	BOOST_CHECK(!first.ok);
	BOOST_CHECK_EQUAL(first.code, "failed");
	BOOST_CHECK_EQUAL(first.error, "boom");
	const cmd::result second = thrown_odd.get();
	BOOST_CHECK_EQUAL(second.code, "failed");
	BOOST_CHECK(second.error.find("unknown exception") != std::string::npos);
	// The drain survived: the call posted behind them still ran.
	BOOST_CHECK(after.get().ok);
	BOOST_CHECK_EQUAL(later_runs, 1);
}

BOOST_AUTO_TEST_CASE(CallNowOrPost_OnTheMainThreadRunsAtOnce_SoWaitingOnTheFutureCannotDeadlock)
{
	int runs = 0;
	cmd::command_desc desc;
	desc.id = "test.now";
	desc.invoke = [&runs](const cmd::call&) { ++runs; return cmd::result::success(); };
	auto reg = commands.add(std::move(desc));

	auto outcome = queue.call_now_or_post("test.now", json_of("{}"), cmd::origin::test());
	BOOST_CHECK_EQUAL(runs, 1); // before any drain
	BOOST_REQUIRE(is_ready(outcome));
	BOOST_CHECK(outcome.get().ok);
	BOOST_CHECK_EQUAL(tasks.pending_main(ds::main_class::general), 0u);
}

BOOST_AUTO_TEST_CASE(CallNowOrPost_OnTheMainThreadAnswersUnknownAndThrowingCommands)
{
	cmd::command_desc desc;
	desc.id = "test.now_throws";
	desc.invoke = [](const cmd::call&) -> cmd::result { throw std::runtime_error("inline boom"); };
	auto reg = commands.add(std::move(desc));

	auto unknown = queue.call_now_or_post("test.nothing", json_of("{}"), cmd::origin::test());
	auto thrown = queue.call_now_or_post("test.now_throws", json_of("{}"), cmd::origin::test());
	BOOST_REQUIRE(is_ready(unknown));
	BOOST_REQUIRE(is_ready(thrown));
	BOOST_CHECK_EQUAL(unknown.get().code, "not_found");
	const cmd::result result = thrown.get();
	BOOST_CHECK_EQUAL(result.code, "failed");
	BOOST_CHECK_EQUAL(result.error, "inline boom");
}

BOOST_AUTO_TEST_CASE(CallNowOrPost_FromAWorker_IsAPostAndWaitsForTheDrain)
{
	std::thread::id ran_on;
	cmd::command_desc desc;
	desc.id = "test.later";
	desc.invoke = [&ran_on](const cmd::call&) { ran_on = std::this_thread::get_id(); return cmd::result::success(); };
	auto reg = commands.add(std::move(desc));

	std::future<cmd::result> outcome;
	std::thread worker([&] { outcome = queue.call_now_or_post("test.later", json_of("{}"), cmd::origin::test()); });
	worker.join();
	BOOST_REQUIRE(outcome.valid());
	BOOST_CHECK(!is_ready(outcome));

	drain();
	BOOST_REQUIRE(is_ready(outcome));
	BOOST_CHECK(outcome.get().ok);
	BOOST_CHECK(ran_on == std::this_thread::get_id());
}

BOOST_AUTO_TEST_CASE(CallNowOrPost_RunsAheadOfCallsAlreadyWaitingInTheQueue)
{
	std::vector<std::string> order;
	cmd::command_desc desc;
	desc.id = "test.tag";
	desc.args_schema = json_of(R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})");
	desc.invoke = [&order](const cmd::call& request) {
		order.emplace_back(request.args.as_object().at("name").as_string());
		return cmd::result::success();
	};
	auto reg = commands.add(std::move(desc));

	auto queued = queue.post("test.tag", json_of(R"({"name":"queued"})"), cmd::origin::test());
	auto inline_call = queue.call_now_or_post("test.tag", json_of(R"({"name":"inline"})"), cmd::origin::test());
	drain();
	BOOST_CHECK(queued.get().ok);
	BOOST_CHECK(inline_call.get().ok);
	const std::vector<std::string> expected{ "inline", "queued" };
	BOOST_TEST(order == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Confirmation_TravelsAsAParameterOfItsOwn_NotInTheArguments)
{
	struct ask_gate final : cmd::policy_gate
	{
		cmd::policy_decision decide(const cmd::command_desc&, const cmd::call&) override { return cmd::policy_decision::ask("x"); }
		bool can_ask() const override { return true; }
	};
	commands.set_policy_gate(std::make_shared<ask_gate>());
	int runs = 0;
	cmd::command_desc desc;
	desc.id = "test.guarded";
	desc.invoke = [&runs](const cmd::call&) {
		++runs;
		return cmd::result::success();
	};
	auto registration = commands.add(std::move(desc));

	auto asked = queue.call_now_or_post("test.guarded", boost::json::object{}, cmd::origin::agent("s1"));
	const std::string ticket = std::string{ asked.get().value.as_object().at("ticket").as_string().c_str() };
	commands.approve(ticket);

	auto smuggled = queue.call_now_or_post("test.guarded", boost::json::object{ { "confirmation", ticket } }, cmd::origin::agent("s1"));
	BOOST_TEST(smuggled.get().code == cmd::codes::confirmation_required);
	BOOST_TEST(runs == 0);
	auto presented = queue.call_now_or_post("test.guarded", boost::json::object{}, cmd::origin::agent("s1"), 0, ticket);
	BOOST_TEST(presented.get().ok);
	BOOST_TEST(runs == 1);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(CmdMainThreadQueueLifetimeTests)

BOOST_AUTO_TEST_CASE(QueueDestroyedBeforeTheDrain_FailsTheCallInsteadOfTouchingTheRegistry)
{
	ds::task_system tasks{ test_util::manual_tasks() };
	cmd::command_registry commands;
	int runs = 0;
	cmd::command_desc desc;
	desc.id = "test.never";
	desc.invoke = [&runs](const cmd::call&) { ++runs; return cmd::result::success(); };
	auto reg = commands.add(std::move(desc));

	std::future<cmd::result> outcome;
	{
		cmd::main_thread_queue queue(commands, tasks);
		outcome = queue.post("test.never", json_of("{}"), cmd::origin::test());
	}
	tasks.drain_main(std::chrono::hours(1));

	const cmd::result result = outcome.get();
	BOOST_CHECK(!result.ok);
	BOOST_CHECK_EQUAL(result.code, "failed");
	BOOST_CHECK_EQUAL(runs, 0);
}

BOOST_AUTO_TEST_CASE(TaskSystemShutdown_FailsPendingCallsInsteadOfBreakingThePromise)
{
	auto tasks = std::make_unique<ds::task_system>(test_util::manual_tasks());
	cmd::command_registry commands;
	std::future<cmd::result> outcome;
	{
		cmd::main_thread_queue queue(commands, *tasks);
		outcome = queue.post("test.anything", json_of("{}"), cmd::origin::test());
		tasks.reset(); // drops the main queue without calling it
	}
	const cmd::result result = outcome.get();
	BOOST_CHECK(!result.ok);
	BOOST_CHECK_EQUAL(result.code, "failed");
}

BOOST_AUTO_TEST_SUITE_END()
