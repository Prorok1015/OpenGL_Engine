#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

// resource_system on a threaded task_system: the cache, change signals and aliases under real contention
// (the TSan job runs these), and the delivery thread of res_handle::then.

namespace {

// Records which thread constructed it, i.e. which thread ran the adapter.
struct probe_resource : res::text_resource
{
	probe_resource(const res::tag& tag, const std::vector<std::byte>& bytes)
		: res::text_resource(tag, bytes)
		, built_on_worker(ds::task_system::on_worker())
	{
	}

	bool built_on_worker;
};

struct probe_adapter : core::res::adapter_interface
{
	static inline auto INFO = res::adapter_info::make<probe_resource>();

	std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw) const override
	{
		return std::make_shared<probe_resource>(tag, raw);
	}

	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>& resource) const override
	{
		return test_util::to_bytes(std::static_pointer_cast<probe_resource>(resource)->str());
	}
};

struct threaded_env
{
	explicit threaded_env(ds::task_system_config config = {})
		: tasks(std::move(config))
	{
		std::filesystem::create_directories("./res");
		std::filesystem::create_directories("./sandbox");
		rs = std::make_unique<res::resource_system>(tasks);
		rs->unregistrate_resolver(res::tag::memory);
		ram = &rs->registrate_resolver<test_util::ram_resolver>(res::tag::memory);
		rs->registrate_adapter<probe_adapter>(probe_adapter::INFO);
	}

	~threaded_env()
	{
		rs.reset();
	}

	// ram_resolver is not thread safe for writes: store everything before any thread starts.
	res::tag put(std::string_view name, std::string_view content)
	{
		const res::tag tag{ std::string("memory://c/") + std::string(name) };
		rs->store(tag, test_util::to_bytes(content));
		return tag;
	}

	void drain_all_main()
	{
		while (tasks.pending_main(ds::main_class::general) > 0) {
			tasks.drain_main();
		}
	}

	ds::task_system tasks; // threaded; outlives rs
	std::unique_ptr<res::resource_system> rs;
	test_util::ram_resolver* ram = nullptr;
};

// A resource whose adapter needs another resource: the nested require_sync that #134b replaces with a graph.
struct child_resource : res::text_resource
{
	using res::text_resource::text_resource;
};

struct child_adapter : core::res::adapter_interface
{
	static inline auto INFO = res::adapter_info::make<child_resource>();

	explicit child_adapter(res::resource_system& rs) : m_rs(rs) {}

	std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw) const override
	{
		const std::string parent_tag(reinterpret_cast<const char*>(raw.data()), raw.size());
		auto parent = m_rs.require_sync<res::text_resource>(res::tag{ parent_tag }).get_sync();
		if (!parent) {
			throw std::runtime_error("parent failed to load");
		}
		return std::make_shared<child_resource>(tag, test_util::to_bytes(parent->str() + "+child"));
	}

	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }

	res::resource_system& m_rs;
};

// A regression here is a deadlock: end the process with a message instead of letting CI hang until its timeout.
struct deadline_guard
{
	explicit deadline_guard(int seconds)
		: thread([this, seconds] {
			for (int i = 0; i < seconds * 20 && !done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
			if (!done.load()) {
				egLOG_ERROR("test", "deadlock: the test did not finish in {} s", seconds);
				std::_Exit(3);
			}
		})
	{
	}
	~deadline_guard()
	{
		done = true;
		thread.join();
	}
	std::atomic<bool> done{ false };
	std::thread thread;
};

template<class F>
void run_threads(int count, F&& body)
{
	std::vector<std::thread> threads;
	threads.reserve(static_cast<std::size_t>(count));
	for (int i = 0; i < count; ++i) {
		threads.emplace_back(body, i);
	}
	for (auto& thread : threads) {
		thread.join();
	}
}

}

// Two compute threads: easy to exhaust by workers that wait for a load queued behind them.
struct two_thread_env : threaded_env
{
	static ds::task_system_config config()
	{
		ds::task_system_config c;
		c.compute_threads = 2;
		return c;
	}

	two_thread_env() : threaded_env(config())
	{
		rs->registrate_adapter<child_adapter>(child_adapter::INFO, std::ref(*rs));
	}

	std::vector<res::res_handle<child_resource>> request_children(int count, const res::tag& parent)
	{
		std::vector<res::tag> tags; // all stored before the first load starts: ram_resolver has no locking
		for (int i = 0; i < count; ++i) {
			tags.push_back(put("child_" + std::to_string(i) + ".bin", parent.string()));
		}
		std::vector<res::res_handle<child_resource>> handles;
		for (const auto& tag : tags) {
			handles.push_back(rs->require<child_resource>(tag));
		}
		return handles;
	}

	static void expect_all_loaded(const std::vector<res::res_handle<child_resource>>& handles)
	{
		for (const auto& handle : handles) {
			for (int i = 0; i < 4000 && !handle.is_ready() && !handle.has_error(); ++i) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			BOOST_REQUIRE(handle.is_ready());
			BOOST_TEST(handle.get()->str() == "parent data+child");
		}
	}
};

BOOST_FIXTURE_TEST_SUITE(ResSystemConcurrencyTests, threaded_env)

BOOST_AUTO_TEST_CASE(Require_RacingSignalChanged_AlwaysYieldsTheResourceAndEveryWatcherCallArrives)
{
	const auto tag = put("race.txt", "payload");
	constexpr int signals = 300;
	constexpr int requesters = 4;
	constexpr int rounds = 300;

	std::atomic<int> watcher_calls{ 0 };
	int owner = 0;
	rs->watch(tag, &owner, [&](const res::tag&) { ++watcher_calls; });

	std::atomic<int> bad{ 0 };
	std::atomic<int> started{ 0 };
	run_threads(requesters + 1, [&](int index) {
		++started;
		while (started < requesters + 1) std::this_thread::yield();
		if (index == requesters) {
			for (int i = 0; i < signals; ++i) rs->signal_changed(tag);
			return;
		}
		for (int i = 0; i < rounds; ++i) {
			auto ptr = rs->require<res::text_resource>(tag).get_sync();
			if (!ptr || ptr->str() != "payload") ++bad;
		}
	});

	BOOST_TEST(bad.load() == 0);
	drain_all_main();
	BOOST_TEST(watcher_calls.load() == signals);
	rs->unwatch(tag, &owner);
}

BOOST_AUTO_TEST_CASE(Require_RacingRegisterAlias_NeverSeesAHalfCopiedEntry)
{
	const auto original = put("alias_orig.txt", "same");
	const auto target = put("alias_target.txt", "same");
	constexpr int requesters = 4;
	constexpr int rounds = 300;

	std::atomic<int> bad{ 0 };
	std::atomic<int> started{ 0 };
	run_threads(requesters + 1, [&](int index) {
		++started;
		while (started < requesters + 1) std::this_thread::yield();
		if (index == requesters) {
			for (int i = 0; i < rounds; ++i) rs->register_alias(original, target);
			return;
		}
		for (int i = 0; i < rounds; ++i) {
			auto ptr = rs->require<res::text_resource>((i + index) % 2 ? original : target).get_sync();
			if (!ptr || ptr->str() != "same") ++bad;
		}
	});

	BOOST_TEST(bad.load() == 0);
	BOOST_TEST(rs->resolve_alias(original) == target);
	BOOST_TEST(rs->require_sync<res::text_resource>(original).get_control_block()
		== rs->require_sync<res::text_resource>(target).get_control_block());
}

BOOST_AUTO_TEST_CASE(Require_RacingSignalChangedAndUnpin_KeepsTheIndexConsistent)
{
	const auto tag = put("pin_race.txt", "pinned");
	constexpr int rounds = 300;

	std::atomic<int> bad{ 0 };
	std::atomic<int> started{ 0 };
	run_threads(4, [&](int index) {
		++started;
		while (started < 4) std::this_thread::yield();
		for (int i = 0; i < rounds; ++i) {
			switch (index) {
			case 0: rs->signal_changed(tag); break;
			case 1: rs->unpin_resource(tag); break;
			case 2: rs->warmup<res::text_resource>(tag); break;
			default: {
				auto ptr = rs->require<res::text_resource>(tag).get_sync();
				if (!ptr || ptr->str() != "pinned") ++bad;
			}
			}
		}
	});

	BOOST_TEST(bad.load() == 0);
	drain_all_main();
}

BOOST_AUTO_TEST_CASE(Then_RunsOnTheMainThreadWhileTheAdapterRanOnAPoolThread)
{
	const auto tag = put("probe.txt", "probe");
	const auto main_thread = std::this_thread::get_id();
	ds::lifetime owner;

	std::atomic<bool> done{ false };
	std::thread::id callback_thread;
	bool callback_on_worker = true;
	bool built_on_worker = false;

	rs->require<probe_resource>(tag).then(owner, [&](probe_resource& r) {
		callback_thread = std::this_thread::get_id();
		callback_on_worker = ds::task_system::on_worker();
		built_on_worker = r.built_on_worker;
		done = true;
	});

	// Only this thread drains the main queue, so the callback cannot run anywhere else.
	for (int i = 0; i < 5000 && !done.load(); ++i) {
		tasks.drain_main();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	BOOST_REQUIRE(done.load());
	BOOST_TEST((callback_thread == main_thread));
	BOOST_TEST(!callback_on_worker);
	BOOST_TEST(built_on_worker);
}

BOOST_AUTO_TEST_CASE(Destruction_WithLoadsInFlightWaitsForThemAndDropsTheRest)
{
	std::vector<res::tag> tags;
	for (int i = 0; i < 100; ++i) {
		tags.push_back(put("bulk_" + std::to_string(i) + ".txt", "x"));
	}
	std::vector<res::res_handle<res::text_resource>> handles;
	for (const auto& tag : tags) {
		handles.push_back(rs->require<res::text_resource>(tag));
	}
	rs.reset(); // cancels what has not started, waits for what has: no task may touch the dead system
	BOOST_TEST(tasks.pending_main(ds::main_class::general) == 0u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(ResSystemNestedLoadTests, two_thread_env)

BOOST_AUTO_TEST_CASE(TwoChildrenRacingOnTheSameUnloadedParent_BothGetItsData)
{
	deadline_guard deadline(60);
	const auto parent = put("parent.txt", "parent data");
	const auto children = request_children(2, parent);
	expect_all_loaded(children);
}

BOOST_AUTO_TEST_CASE(ManyChildrenWaitingForAParentQueuedBehindThem_DoNotStarveTheLane)
{
	deadline_guard deadline(60);
	const auto parent = put("parent.txt", "parent data");

	// Both compute threads are held, so the queue order below is exactly the order the tasks run in.
	std::atomic<bool> release{ false };
	std::atomic<int> held{ 0 };
	for (int i = 0; i < 2; ++i) {
		tasks.run(ds::lane::compute, ds::task_desc{ "gate", "test", false }, [&](ds::task_context&) {
			++held;
			while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		});
	}
	while (held.load() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));

	// The children go first; the parent's own load task is queued behind all of them, and its block already exists
	// when they run, so their require_sync has to wait for it instead of loading it inline.
	const auto children = request_children(5, parent);
	auto parent_handle = rs->require<res::text_resource>(parent);
	release = true;

	expect_all_loaded(children);
	BOOST_TEST(parent_handle.is_ready());
}

BOOST_AUTO_TEST_SUITE_END()
