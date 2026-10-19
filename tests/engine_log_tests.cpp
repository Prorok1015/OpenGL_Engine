#include <boost/test/unit_test.hpp>
#include <snk/common/logger/engine_log.h>
#include "test_temp_dir.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <charconv>
#include <system_error>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

struct captured_line
{
	std::string category;
	engine::log_level level;
	std::string message;
};

// Installs a capturing sink and always removes it again, so later tests never log into a dangling lambda.
struct capture
{
	capture()
	{
		sink = engine::add_log_sink([this](std::string_view cat, engine::log_level lvl, std::string_view msg) {
			lines.push_back({ std::string(cat), lvl, std::string(msg) });
		});
	}
	std::vector<captured_line> lines;
	engine::log_sink_handle sink; // last: removed first
};

// Sets the journal capacity for one test and puts the old one back.
struct journal_capacity_guard
{
	// Also keeps every level, so a test does not depend on the logger's level in this build.
	explicit journal_capacity_guard(std::size_t entries)
		: previous(engine::log_journal_capacity())
		, previous_level(engine::log_journal_min_level())
	{
		engine::set_log_journal_capacity(entries);
		engine::set_log_journal_min_level(engine::log_level::trace);
	}
	~journal_capacity_guard()
	{
		engine::set_log_journal_capacity(previous);
		engine::set_log_journal_min_level(previous_level);
	}
	std::size_t previous;
	std::optional<engine::log_level> previous_level;
};

std::vector<std::string> texts_of(const engine::log_page& page)
{
	std::vector<std::string> out;
	for (const auto& entry : page.entries) out.push_back(entry.text);
	return out;
}

}

BOOST_AUTO_TEST_SUITE(EngineLogTests)

BOOST_AUTO_TEST_CASE(Callback_ReceivesCategoryLevelAndFormattedMessage)
{
	capture cap;
	egLOG_WARN("covtest/cb", "x={} y={} z={:.1f}", 1, "two", 3.25);
	BOOST_REQUIRE(cap.lines.size() == 1u);
	BOOST_TEST(cap.lines[0].category == "covtest/cb");
	BOOST_TEST((cap.lines[0].level == engine::log_level::warn));
	BOOST_TEST(cap.lines[0].message == "x=1 y=two z=3.2");
}

BOOST_AUTO_TEST_CASE(Macros_MapToTheirLevels_AndEgLogIsInfo)
{
	capture cap;
	egLOG_TRACE("covtest/lvl", "t");
	egLOG_DEBUG("covtest/lvl", "d");
	egLOG_INFO("covtest/lvl", "i");
	egLOG_WARN("covtest/lvl", "w");
	egLOG_ERROR("covtest/lvl", "e");
	egLOG("covtest/lvl", "plain {}", 7);

	BOOST_REQUIRE(cap.lines.size() == 6u);
	const engine::log_level expected[] = {
		engine::log_level::trace, engine::log_level::debug, engine::log_level::info,
		engine::log_level::warn, engine::log_level::error, engine::log_level::info };
	for (size_t i = 0; i < 6; ++i) BOOST_TEST((cap.lines[i].level == expected[i]));
	BOOST_TEST(cap.lines[5].message == "plain 7");
}

BOOST_AUTO_TEST_CASE(Callback_SeesMessagesEvenWhenTheCategoryLevelFiltersThemFromSinks)
{
	// Pins current behavior: the editor-console callback is not subject to level filtering.
	capture cap;
	engine::set_category_level("covtest/quiet", engine::log_level::error);
	egLOG_DEBUG("covtest/quiet", "hidden from sinks");
	BOOST_REQUIRE(cap.lines.size() == 1u);
	BOOST_TEST(cap.lines[0].message == "hidden from sinks");
}

BOOST_AUTO_TEST_CASE(Sink_RemovedByResettingItsHandle)
{
	std::vector<std::string> seen;
	auto handle = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view m) { seen.emplace_back(m); });
	BOOST_TEST(handle.active());
	egLOG_INFO("covtest/cb2", "one");
	handle.reset();
	BOOST_TEST(!handle.active());
	egLOG_INFO("covtest/cb2", "two");
	BOOST_TEST((seen == std::vector<std::string>{ "one" }));
}

BOOST_AUTO_TEST_CASE(Sinks_SeveralAtOnce_AllSeeEveryMessageInTheOrderTheyWereAdded)
{
	std::vector<std::string> order;
	auto first = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view m) { order.push_back("a:" + std::string(m)); });
	auto second = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view m) { order.push_back("b:" + std::string(m)); });
	egLOG_INFO("covtest/multi", "x");
	BOOST_TEST((order == std::vector<std::string>{ "a:x", "b:x" }));

	first.reset();
	order.clear();
	egLOG_INFO("covtest/multi", "y");
	BOOST_TEST((order == std::vector<std::string>{ "b:y" }));
}

BOOST_AUTO_TEST_CASE(Sink_HandleIsMovable_AndANullCallbackAddsNothing)
{
	int calls = 0;
	auto original = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view) { ++calls; });
	engine::log_sink_handle moved = std::move(original);
	BOOST_TEST(!original.active());
	BOOST_TEST(moved.active());
	egLOG_INFO("covtest/move", "one");
	BOOST_TEST(calls == 1);

	engine::log_sink_handle assigned;
	assigned = std::move(moved);
	egLOG_INFO("covtest/move", "two");
	BOOST_TEST(calls == 2);

	BOOST_TEST(!engine::add_log_sink(nullptr).active());
	assigned.reset();
	egLOG_INFO("covtest/move", "three");
	BOOST_TEST(calls == 2);
}

BOOST_AUTO_TEST_CASE(Sink_MayRemoveItselfWhileItRuns)
{
	int calls = 0;
	engine::log_sink_handle handle;
	handle = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view) {
		++calls;
		handle.reset();
	});
	egLOG_INFO("covtest/self_remove", "one");
	egLOG_INFO("covtest/self_remove", "two");
	BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_CASE(Sink_MayRemoveAnotherSinkFromInsideItsCallback)
{
	int b_calls = 0;
	engine::log_sink_handle b = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view) { ++b_calls; });
	engine::log_sink_handle a = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view) { b.reset(); });
	egLOG_INFO("covtest/remove_other", "one"); // b runs, then a removes it
	egLOG_INFO("covtest/remove_other", "two");
	BOOST_TEST(b_calls == 1);
	BOOST_TEST(!b.active());
}

BOOST_AUTO_TEST_CASE(Sink_HandleOutlivingTheLoggerShutdownIsHarmless)
{
	int calls = 0;
	auto handle = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view) { ++calls; });
	engine::shutdown_logger();
	egLOG_INFO("covtest/after_shutdown2", "gone");
	BOOST_TEST(calls == 0); // shutdown drops every sink
	BOOST_CHECK_NO_THROW(handle.reset());
}

BOOST_AUTO_TEST_CASE(CategoryFile_ReceivesOnlyItsCategoryWithFormattedLine)
{
	test_util::temp_dir dir("engine_log");
	auto file_a = dir.path() / "a.log";
	auto file_b = dir.path() / "b.log";
	engine::set_category_file("covtest/file_a", file_a.string());
	engine::set_category_file("covtest/file_b", file_b.string());

	egLOG_WARN("covtest/file_a", "hello {}", "file");   // warn+ is flushed immediately

	auto slurp = [](const std::filesystem::path& p) {
		std::ifstream in(p);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	};
	auto a = slurp(file_a);
	BOOST_TEST(a.find("[warning] [covtest/file_a] hello file") != std::string::npos);
	BOOST_TEST(slurp(file_b).find("hello") == std::string::npos);
	// (the sinks keep the files open until process exit; temp_dir removal ignores that on POSIX)
}

BOOST_AUTO_TEST_CASE(LoggingAfterShutdownReinitializes)
{
	capture cap;
	engine::shutdown_logger();
	capture cap2;
	BOOST_CHECK_NO_THROW(egLOG_INFO("covtest/after_shutdown", "still alive"));
	BOOST_TEST(cap2.lines.size() == 1u);
}

BOOST_AUTO_TEST_CASE(GlobalLevel_CanBeRaisedAndRestoredWithoutAffectingCallbackDelivery)
{
	capture cap;
	engine::set_log_level(engine::log_level::error);
	egLOG_INFO("covtest/global", "filtered by sinks");
	engine::set_log_level(engine::log_level::info);
	egLOG_INFO("covtest/global", "delivered");
	BOOST_TEST(cap.lines.size() == 2u);
}

// Regression guard for the startup crash (#83): the editor installs its console sink on the main
// thread while resource/io workers are already logging. Swapping the std::function during a call
// used to tear it (crash, or a TSan data race on the callback).
BOOST_AUTO_TEST_CASE(Sinks_CanBeAddedAndRemovedWhileOtherThreadsLog)
{
	engine::set_log_level(engine::log_level::error); // keep the spin loop out of the sinks
	std::atomic<bool> stop{ false };
	std::atomic<int> delivered{ 0 };
	std::thread logger_thread([&] {
		while (!stop.load(std::memory_order_acquire)) {
			engine::log_message("covtest/race", engine::log_level::info, "tick");
		}
	});

	for (int i = 0; i < 20000; ++i) {
		auto handle = engine::add_log_sink([&delivered](std::string_view, engine::log_level, std::string_view) {
			delivered.fetch_add(1, std::memory_order_relaxed);
		});
		if (i % 8 == 0) handle.reset();
	}
	stop.store(true, std::memory_order_release);
	logger_thread.join();
	engine::set_log_level(engine::log_level::info);

	// Every handle is gone: nothing may be called any more.
	const int settled = delivered.load();
	engine::log_message("covtest/race", engine::log_level::info, "after");
	BOOST_TEST(delivered.load() == settled);
}

// reset() and shutdown_logger() promise to return only after a call in flight on another thread has ended. This checks
// that `finished` (set at the end of the call) is already true when they return. The gate opens just before the
// call, so a broken wait can still pass if the sink wins the race: the check never fails on a correct build, and
// catches a missing wait only most of the time.
BOOST_AUTO_TEST_CASE(Sink_ResetWaitsForACallInFlightOnAnotherThread)
{
	std::atomic<bool> inside{ false };
	std::atomic<bool> about_to_reset{ false };
	std::atomic<bool> finished{ false };
	auto handle = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view) {
		inside.store(true);
		while (!about_to_reset.load()) std::this_thread::yield();
		std::this_thread::yield();
		finished.store(true);
	});
	std::thread logger_thread([] { engine::log_message("covtest/inflight", engine::log_level::info, "x"); });
	while (!inside.load()) std::this_thread::yield();

	about_to_reset.store(true);
	handle.reset();
	BOOST_TEST(finished.load());
	logger_thread.join();
}

BOOST_AUTO_TEST_CASE(ShutdownLogger_WaitsForACallInFlightOnAnotherThread)
{
	std::atomic<bool> inside{ false };
	std::atomic<bool> about_to_shut_down{ false };
	std::atomic<bool> finished{ false };
	auto handle = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view) {
		inside.store(true);
		while (!about_to_shut_down.load()) std::this_thread::yield();
		std::this_thread::yield();
		finished.store(true);
	});
	std::thread logger_thread([] { engine::log_message("covtest/inflight2", engine::log_level::info, "x"); });
	while (!inside.load()) std::this_thread::yield();

	about_to_shut_down.store(true);
	engine::shutdown_logger();
	BOOST_TEST(finished.load());
	logger_thread.join();
}

// The header promises a sink may log: that message reaches the journal, not a sink, so two sinks that log from inside
// cannot take each other's locks in opposite orders.
BOOST_AUTO_TEST_CASE(Sink_ThatLogs_ReachesTheJournalButNoSink)
{
	journal_capacity_guard guard(100);
	std::vector<std::string> seen_a;
	std::vector<std::string> seen_b;
	auto a = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view m) {
		seen_a.emplace_back(m);
		if (m == "outer") egLOG_INFO("covtest/nest", "from a");
	});
	auto b = engine::add_log_sink([&](std::string_view, engine::log_level, std::string_view m) {
		seen_b.emplace_back(m);
		if (m == "outer") egLOG_INFO("covtest/nest", "from b");
	});
	const std::uint64_t first = engine::log_journal_next_seq();
	egLOG_INFO("covtest/nest", "outer");

	BOOST_TEST((seen_a == std::vector<std::string>{ "outer" }));
	BOOST_TEST((seen_b == std::vector<std::string>{ "outer" }));
	engine::log_query query;
	query.since_seq = first;
	query.category = "covtest/nest";
	BOOST_TEST((texts_of(engine::read_log_journal(query)) == std::vector<std::string>{ "outer", "from a", "from b" }));

	// The depth is per call: the next message from outside reaches the sinks again.
	egLOG_INFO("covtest/nest", "outer");
	BOOST_TEST(seen_a.size() == 2u);
}

BOOST_AUTO_TEST_CASE(Journal_KeepsWhatTheLoggerLetsThrough_UnlessAMinLevelSaysOtherwise)
{
	journal_capacity_guard guard(100);
	engine::set_log_journal_min_level(std::nullopt);
	const engine::log_level previous_level = engine::current_log_level();
	engine::set_log_level(engine::log_level::error);
	capture cap;
	const std::uint64_t first = engine::log_journal_next_seq();
	egLOG_INFO("covtest/jlvl", "quiet");
	egLOG_ERROR("covtest/jlvl", "loud");

	engine::log_query query;
	query.since_seq = first;
	query.category = "covtest/jlvl";
	BOOST_TEST((texts_of(engine::read_log_journal(query)) == std::vector<std::string>{ "loud" }));
	BOOST_TEST(cap.lines.size() == 2u); // sinks still see everything

	engine::set_log_journal_min_level(engine::log_level::trace);
	egLOG_INFO("covtest/jlvl", "kept");
	engine::set_log_journal_min_level(engine::log_level::warn);
	egLOG_INFO("covtest/jlvl", "below the floor");
	egLOG_WARN("covtest/jlvl", "above the floor");
	BOOST_TEST((texts_of(engine::read_log_journal(query)) == std::vector<std::string>{ "loud", "kept", "above the floor" }));
	BOOST_TEST((engine::log_journal_min_level() == engine::log_level::warn));

	engine::set_log_level(previous_level);
}

BOOST_AUTO_TEST_CASE(Journal_KeepsSeqTimeLevelCategoryAndText)
{
	const auto before = std::chrono::system_clock::now();
	const std::uint64_t first = engine::log_journal_next_seq();
	egLOG_WARN("covtest/journal", "value={}", 42);
	egLOG_ERROR("covtest/journal", "second");
	const auto after = std::chrono::system_clock::now();

	engine::log_query query;
	query.since_seq = first;
	query.category = "covtest/journal";
	const auto page = engine::read_log_journal(query);
	BOOST_REQUIRE(page.entries.size() == 2u);
	BOOST_TEST(!page.dropped);
	BOOST_TEST(page.entries[0].seq == first);
	BOOST_TEST(page.entries[1].seq == first + 1);
	BOOST_TEST((page.entries[0].level == engine::log_level::warn));
	BOOST_TEST((page.entries[1].level == engine::log_level::error));
	BOOST_TEST(page.entries[0].category == "covtest/journal");
	BOOST_TEST(page.entries[0].text == "value=42");
	BOOST_TEST((page.entries[0].time >= before && page.entries[1].time <= after));
	BOOST_TEST(page.next_seq == first + 2);
}

BOOST_AUTO_TEST_CASE(Journal_RingWrapsAndReportsWhatWasDropped)
{
	journal_capacity_guard guard(5);
	const std::uint64_t first = engine::log_journal_next_seq();
	for (int i = 0; i < 8; ++i) egLOG_INFO("covtest/ring", "m{}", i);

	engine::log_query query;
	query.category = "covtest/ring";
	const auto all = engine::read_log_journal(query);
	BOOST_TEST((texts_of(all) == std::vector<std::string>{ "m3", "m4", "m5", "m6", "m7" }));
	BOOST_TEST(!all.dropped); // asked for "whatever is there"
	BOOST_TEST(all.next_seq == first + 8);

	query.since_seq = first; // m0 is gone
	const auto stale = engine::read_log_journal(query);
	BOOST_TEST(stale.dropped);
	BOOST_TEST(stale.entries.size() == 5u);

	query.since_seq = first + 3; // m3 is the oldest left
	const auto exact = engine::read_log_journal(query);
	BOOST_TEST(!exact.dropped);
	BOOST_TEST(exact.entries.size() == 5u);

	query.since_seq = first + 6;
	BOOST_TEST((texts_of(engine::read_log_journal(query)) == std::vector<std::string>{ "m6", "m7" }));
}

BOOST_AUTO_TEST_CASE(Journal_SinceBeyondTheEnd_IsEmptyAndPointsAtTheNextMessage)
{
	const std::uint64_t next = engine::log_journal_next_seq();
	engine::log_query query;
	query.since_seq = next + 100;
	const auto page = engine::read_log_journal(query);
	BOOST_TEST(page.entries.empty());
	BOOST_TEST(!page.dropped);
	BOOST_TEST(page.next_seq == next);
}

BOOST_AUTO_TEST_CASE(Journal_ShrinkingTheCapacityDropsTheOldest)
{
	journal_capacity_guard guard(100);
	for (int i = 0; i < 6; ++i) egLOG_INFO("covtest/shrink", "s{}", i);
	engine::set_log_journal_capacity(2);
	BOOST_TEST(engine::log_journal_capacity() == 2u);

	engine::log_query query;
	query.category = "covtest/shrink";
	BOOST_TEST((texts_of(engine::read_log_journal(query)) == std::vector<std::string>{ "s4", "s5" }));
}

BOOST_AUTO_TEST_CASE(Journal_CapacityZeroTurnsItOffButSequenceKeepsCounting)
{
	journal_capacity_guard guard(0);
	const std::uint64_t first = engine::log_journal_next_seq();
	egLOG_INFO("covtest/off", "lost");
	BOOST_TEST(engine::log_journal_next_seq() == first + 1);

	engine::log_query query;
	query.since_seq = first;
	const auto page = engine::read_log_journal(query);
	BOOST_TEST(page.entries.empty());
	BOOST_TEST(page.dropped);
}

BOOST_AUTO_TEST_CASE(Journal_PagesContinueWhereThePreviousOneStopped)
{
	journal_capacity_guard guard(100);
	const std::uint64_t first = engine::log_journal_next_seq();
	for (int i = 0; i < 5; ++i) egLOG_INFO("covtest/page", "p{}", i);

	engine::log_query query;
	query.since_seq = first;
	query.category = "covtest/page";
	query.limit = 2;
	std::vector<std::string> seen;
	for (int loops = 0; loops < 10; ++loops) {
		const auto page = engine::read_log_journal(query);
		for (const auto& text : texts_of(page)) seen.push_back(text);
		if (page.entries.size() < query.limit) break;
		query.since_seq = page.next_seq;
	}
	BOOST_TEST((seen == std::vector<std::string>{ "p0", "p1", "p2", "p3", "p4" }));
}

BOOST_AUTO_TEST_CASE(Journal_FiltersByLevelAndCategoryBranch)
{
	journal_capacity_guard guard(100);
	const std::uint64_t first = engine::log_journal_next_seq();
	egLOG_DEBUG("covtest/flt", "debug");
	egLOG_WARN("covtest/flt", "warn");
	egLOG_ERROR("covtest/flt/sub", "error");
	egLOG_ERROR("covtest/fltx", "other branch");

	engine::log_query query;
	query.since_seq = first;
	query.category = "covtest/flt";
	BOOST_TEST((texts_of(engine::read_log_journal(query)) == std::vector<std::string>{ "debug", "warn", "error" }));

	query.min_level = engine::log_level::warn;
	BOOST_TEST((texts_of(engine::read_log_journal(query)) == std::vector<std::string>{ "warn", "error" }));

	query.min_level = engine::log_level::error;
	BOOST_TEST((texts_of(engine::read_log_journal(query)) == std::vector<std::string>{ "error" }));

	query.category = "covtest/flt/sub";
	BOOST_TEST((texts_of(engine::read_log_journal(query)) == std::vector<std::string>{ "error" }));
}

BOOST_AUTO_TEST_CASE(Journal_ManyThreadsLoggingKeepEveryMessageWithUniqueIncreasingSeq)
{
	constexpr int threads = 4;
	constexpr int per_thread = 400;
	journal_capacity_guard guard(10000);
	const std::uint64_t first = engine::log_journal_next_seq();

	std::vector<std::thread> workers;
	for (int t = 0; t < threads; ++t) {
		workers.emplace_back([t] {
			for (int i = 0; i < per_thread; ++i) egLOG_INFO("covtest/mt", "t{}:{}", t, i);
		});
	}
	for (auto& worker : workers) worker.join();

	engine::log_query query;
	query.since_seq = first;
	query.category = "covtest/mt";
	query.limit = threads * per_thread;
	const auto page = engine::read_log_journal(query);
	BOOST_REQUIRE(page.entries.size() == static_cast<std::size_t>(threads * per_thread));
	BOOST_TEST(!page.dropped);

	std::vector<int> next_index(threads, 0);
	std::uint64_t previous_seq = 0;
	bool order_ok = true;
	for (const auto& entry : page.entries) {
		if (entry.seq <= previous_seq) order_ok = false;
		previous_seq = entry.seq;
		const std::size_t colon = entry.text.find(':');
		int t = -1, i = -1;
		if (colon == std::string::npos
			|| std::from_chars(entry.text.data() + 1, entry.text.data() + colon, t).ec != std::errc()
			|| std::from_chars(entry.text.data() + colon + 1, entry.text.data() + entry.text.size(), i).ec != std::errc()
			|| t < 0 || t >= threads || next_index[static_cast<std::size_t>(t)]++ != i) order_ok = false;
	}
	BOOST_TEST(order_ok);
}

BOOST_AUTO_TEST_CASE(Journal_SequenceKeepsCountingAcrossShutdown)
{
	egLOG_INFO("covtest/seq", "before");
	const std::uint64_t before = engine::log_journal_next_seq();
	engine::shutdown_logger();
	BOOST_TEST(engine::log_journal_next_seq() == before);

	engine::log_query query;
	query.category = "covtest/seq";
	BOOST_TEST(engine::read_log_journal(query).entries.empty()); // the ring was emptied

	egLOG_INFO("covtest/seq", "after");
	const auto page = engine::read_log_journal(query);
	BOOST_REQUIRE(page.entries.size() == 1u);
	BOOST_TEST(page.entries[0].seq == before);
}

BOOST_AUTO_TEST_SUITE_END()
