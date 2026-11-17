#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <algorithm>
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

// A resource whose adapter needs another resource, declared as a dependency.
struct child_resource : res::text_resource
{
	using res::text_resource::text_resource;
};

struct grand_resource : res::text_resource
{
	using res::text_resource::text_resource;
};

// Its raw data is the tag of the resource it needs.
template<class Out, class Dep>
struct dependent_adapter : core::res::adapter_interface
{
	static inline auto INFO = res::adapter_info::make<Out>();

	static res::tag needed(const std::vector<std::byte>& raw)
	{
		return res::tag{ std::string(reinterpret_cast<const char*>(raw.data()), raw.size()) };
	}

	std::vector<res::res_dep> collect_dependencies(const res::tag&, const std::vector<std::byte>& raw) const override
	{
		return { res::make_dep<Dep>(needed(raw)) };
	}

	std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw, const res::res_deps& deps) const override
	{
		auto parent = deps.get<Dep>(needed(raw));
		if (!parent) {
			throw std::runtime_error("dependency failed to load");
		}
		return std::make_shared<Out>(tag, test_util::to_bytes(parent->str() + (std::is_same_v<Out, child_resource> ? "+child" : "+grand")));
	}

	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }
};

using child_adapter = dependent_adapter<child_resource, res::text_resource>;
using grand_adapter = dependent_adapter<grand_resource, child_resource>;

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

// The text of a shader with its #include "file" lines replaced by the files, which are its dependencies.
struct shader_source_resource : res::text_resource
{
	using res::text_resource::text_resource;
};

struct shader_include_adapter : core::res::adapter_interface
{
	static inline auto INFO = res::adapter_info::make<shader_source_resource>({ "frag" });
	static constexpr std::string_view directive = "#include \"";

	static std::vector<res::tag> includes_of(const res::tag& tag, const std::vector<std::byte>& raw)
	{
		std::vector<res::tag> result;
		const std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
		for (auto at = text.find(directive); at != std::string::npos; at = text.find(directive, at + 1)) {
			const auto begin = at + directive.size();
			const auto end = text.find('"', begin);
			result.push_back(tag + res::tag::make(std::string_view{ text }.substr(begin, end - begin)));
		}
		return result;
	}

	std::vector<res::res_dep> collect_dependencies(const res::tag& tag, const std::vector<std::byte>& raw) const override
	{
		std::vector<res::res_dep> deps;
		for (const auto& include : includes_of(tag, raw)) {
			deps.push_back(res::make_dep<res::text_resource>(include));
		}
		return deps;
	}

	std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw, const res::res_deps& deps) const override
	{
		std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
		for (const auto& include : includes_of(tag, raw)) {
			const auto code = deps.get<res::text_resource>(include);
			if (!code) {
				throw std::runtime_error("include did not load");
			}
			const auto line_begin = text.find(directive);
			const auto line_end = text.find('\n', line_begin);
			text.replace(line_begin, line_end - line_begin, code->str());
		}
		return std::make_shared<shader_source_resource>(tag, test_util::to_bytes(text));
	}

	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }
};

struct graph_test_desc : desc::desc_base
{
	int value = 0;

	void copy_to(desc::desc_base& other) const override
	{
		if (auto* typed = dynamic_cast<graph_test_desc*>(&other)) {
			typed->value = value;
		}
	}
	void deserialize(desc::desc_system&, const boost::json::object& obj) override
	{
		if (obj.contains("value")) value = static_cast<int>(obj.at("value").as_int64());
	}
	void serialize(boost::json::object& obj) const override { obj["value"] = value; }
};

}

// A resource that needs the one its raw data names, which may be a cycle.
struct loop_resource : res::text_resource
{
	using res::text_resource::text_resource;
};

using loop_adapter = dependent_adapter<loop_resource, loop_resource>;

// Few compute threads: easy to exhaust by workers that wait for a load queued behind them.
template<std::size_t threads>
struct pool_env : threaded_env
{
	static ds::task_system_config config()
	{
		ds::task_system_config c;
		c.compute_threads = threads;
		return c;
	}

	pool_env() : threaded_env(config())
	{
		rs->registrate_adapter<child_adapter>(child_adapter::INFO);
		rs->registrate_adapter<grand_adapter>(grand_adapter::INFO);
		rs->registrate_adapter<loop_adapter>(loop_adapter::INFO);
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

	template<class H>
	static void wait_finished(const H& handle)
	{
		for (int i = 0; i < 4000 && !handle.is_ready() && !handle.has_error(); ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		BOOST_REQUIRE(handle.is_ready() || handle.has_error());
	}

	static void expect_all_loaded(const std::vector<res::res_handle<child_resource>>& handles)
	{
		for (const auto& handle : handles) {
			wait_finished(handle);
			BOOST_REQUIRE(handle.is_ready());
			BOOST_TEST(handle.get()->str() == "parent data+child");
		}
	}

	// Holds `count` compute threads until release() so that the queue order is exactly the order of the require calls.
	struct gate
	{
		gate(ds::task_system& tasks, std::size_t count)
			: state(std::make_shared<shared>())
		{
			for (std::size_t i = 0; i < count; ++i) {
				tasks.run(ds::lane::compute, ds::task_desc{ "gate", "test", false }, [state = state](ds::task_context&) {
					++state->held;
					while (!state->open.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
				});
			}
			while (state->held.load() < static_cast<int>(count)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		~gate() { release(); }
		void release() { state->open = true; }

		struct shared
		{
			std::atomic<bool> open{ false };
			std::atomic<int> held{ 0 };
		};
		std::shared_ptr<shared> state;
	};
};

using one_thread_env = pool_env<1>;
using two_thread_env = pool_env<2>;

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

// R needs X, which is still queued behind D, and D needs R. Nothing waits for anything, so each load is a task of
// its own and neither can hold up the other.
BOOST_AUTO_TEST_CASE(WorkerWaitingForAQueuedDependency_DoesNotRunAnUnrelatedLoaderThatNeedsItsOwnResource)
{
	deadline_guard deadline(60);
	const auto x = put("x.txt", "x data");
	const auto r = put("r.bin", x.string());
	const auto d = put("d.bin", r.string());

	// The gates own their flags: a gate task polls for up to a timer tick after its flag is set, which can be after
	// this test has returned. With the flags on this stack the poll read freed stack memory (Release), never saw
	// "open", and ~task_system waited for the gate forever (#300).
	gate first_gate(tasks, 1);
	gate second_gate(tasks, 1);

	// Queue order: R, D, X. Only one worker is freed, so it takes R first.
	auto r_handle = rs->require<child_resource>(r);
	auto d_handle = rs->require<grand_resource>(d);
	auto x_handle = rs->require<res::text_resource>(x);
	first_gate.release();

	for (int i = 0; i < 4000 && !(r_handle.is_ready() && d_handle.is_ready()); ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	second_gate.release();
	BOOST_REQUIRE(r_handle.is_ready());
	BOOST_REQUIRE(d_handle.is_ready());
	BOOST_TEST(r_handle.get()->str() == "x data+child");
	BOOST_TEST(d_handle.get()->str() == "x data+child+grand");
	BOOST_TEST(x_handle.is_ready());
}

BOOST_AUTO_TEST_CASE(ManyChildrenWaitingForAParentQueuedBehindThem_DoNotStarveTheLane)
{
	deadline_guard deadline(60);
	const auto parent = put("parent.txt", "parent data");

	// Both compute threads are held, so the queue order below is exactly the order the tasks run in.
	gate hold(tasks, 2);

	// The children go first; the parent's own load task is queued behind all of them, and its block already exists
	// when they run, so they only note that they wait for it and give the thread back.
	const auto children = request_children(5, parent);
	auto parent_handle = rs->require<res::text_resource>(parent);
	hold.release();

	expect_all_loaded(children);
	BOOST_TEST(parent_handle.is_ready());
}

BOOST_AUTO_TEST_SUITE_END()

// The dependency graph on real threads. A regression is a hang, so each test has a deadline.
BOOST_FIXTURE_TEST_SUITE(ResSystemDependencyGraphTests, one_thread_env)

// The deadlock this replaces: C needs P, P is already queued (behind C), and C's adapter used to wait for it
// on the only thread that could have run it.
BOOST_AUTO_TEST_CASE(ChildQueuedBeforeItsParent_OnOneComputeThread_BothLoad)
{
	deadline_guard deadline(60);
	const auto parent = put("parent.txt", "parent data");
	const auto child = put("child.bin", parent.string());

	gate hold(tasks, 1);
	auto child_handle = rs->require<child_resource>(child);
	auto parent_handle = rs->require<res::text_resource>(parent);
	hold.release();

	wait_finished(child_handle);
	BOOST_REQUIRE(child_handle.is_ready());
	BOOST_TEST(child_handle.get()->str() == "parent data+child");
	BOOST_TEST(parent_handle.is_ready());
}

BOOST_AUTO_TEST_CASE(ChainRequestedFromTheLeaf_OnOneComputeThread_LoadsEveryLevel)
{
	deadline_guard deadline(60);
	const auto text = put("text.txt", "t");
	const auto child = put("child.bin", text.string());
	const auto grand = put("grand.bin", child.string());

	gate hold(tasks, 1);
	auto grand_handle = rs->require<grand_resource>(grand);
	hold.release();

	wait_finished(grand_handle);
	BOOST_REQUIRE(grand_handle.is_ready());
	BOOST_TEST(grand_handle.get()->str() == "t+child+grand");
}

BOOST_AUTO_TEST_CASE(CycleBetweenTwoIndependentRoots_FailsBothWithoutHanging)
{
	deadline_guard deadline(60);
	const auto a = put("a.loop", "memory://c/b.loop");
	const auto b = put("b.loop", "memory://c/a.loop");

	gate hold(tasks, 1);
	auto a_handle = rs->require<loop_resource>(a);
	auto b_handle = rs->require<loop_resource>(b);
	hold.release();

	wait_finished(a_handle);
	wait_finished(b_handle);
	BOOST_TEST(a_handle.has_error());
	BOOST_TEST(b_handle.has_error());
	BOOST_TEST(a_handle.get_control_block()->error_msg.find("dependency cycle") != std::string::npos);
	BOOST_TEST(b_handle.get_control_block()->error_msg.find("dependency cycle") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ResourceThatNeedsItself_FailsWithACycle)
{
	deadline_guard deadline(60);
	const auto self = put("self.loop", "memory://c/self.loop");
	auto handle = rs->require<loop_resource>(self);
	wait_finished(handle);
	BOOST_TEST(handle.has_error());
	BOOST_TEST(handle.get_control_block()->error_msg.find("dependency cycle") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(CycleOfThree_FailsAllAndTheRootThatWaitsForIt)
{
	deadline_guard deadline(60);
	const auto a = put("a.loop", "memory://c/b.loop");
	const auto b = put("b.loop", "memory://c/c.loop");
	const auto c = put("c.loop", "memory://c/a.loop");
	const auto root = put("root.loop", "memory://c/a.loop");

	gate hold(tasks, 1);
	auto root_handle = rs->require<loop_resource>(root);
	auto a_handle = rs->require<loop_resource>(a);
	auto b_handle = rs->require<loop_resource>(b);
	auto c_handle = rs->require<loop_resource>(c);
	hold.release();

	for (const auto& handle : { a_handle, b_handle, c_handle, root_handle }) {
		wait_finished(handle);
		BOOST_TEST(handle.has_error());
		BOOST_TEST(handle.get_control_block()->error_msg.find("dependency cycle") != std::string::npos);
	}
}

BOOST_AUTO_TEST_CASE(WhenAll_WithOneFailingDependency_FailsOnceTheOthersAreDeliveredAndCached)
{
	deadline_guard deadline(60);
	const auto one = put("one.txt", "1");
	const auto two = put("two.txt", "2");
	const res::tag missing{ "memory://c/missing.txt" };

	auto first = rs->require<res::text_resource>(one);
	auto broken = rs->require<res::text_resource>(missing);
	auto last = rs->require<res::text_resource>(two);
	auto all = res::when_all(*rs, first, broken, last);

	bool called = false;
	ds::lifetime owner;
	all.then(owner, [&](res::when_all_result&) { called = true; });

	wait_finished(all);
	BOOST_TEST(all.has_error());
	BOOST_TEST(all.get_control_block()->error_msg.find("missing.txt") != std::string::npos);
	BOOST_TEST(first.is_ready());
	BOOST_TEST(last.is_ready());
	BOOST_TEST(broken.has_error());
	BOOST_TEST(first.get()->str() == "1");
	BOOST_TEST(last.get()->str() == "2");

	// Delivered and cached: asking again returns the very blocks the join waited for.
	BOOST_TEST(rs->require<res::text_resource>(one).get_control_block() == first.get_control_block());
	BOOST_TEST(rs->require<res::text_resource>(two).get_control_block() == last.get_control_block());

	drain_all_main();
	BOOST_TEST(!called); // a failed join never calls back
}

BOOST_AUTO_TEST_CASE(WhenAll_WithEveryDependencyLoaded_IsReadyAndCallsBackOnTheMainThread)
{
	deadline_guard deadline(60);
	const auto one = put("one.txt", "1");
	const auto two = put("two.txt", "2");
	std::vector<res::res_handle<res::text_resource>> handles{ rs->require<res::text_resource>(one), rs->require<res::text_resource>(two) };
	auto all = res::when_all(*rs, handles);

	std::atomic<bool> done{ false };
	bool on_main = false;
	ds::lifetime owner;
	all.then(owner, [&](res::when_all_result&) {
		on_main = !ds::task_system::on_worker();
		done = true;
	});

	for (int i = 0; i < 5000 && !done.load(); ++i) {
		tasks.drain_main();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	BOOST_REQUIRE(done.load());
	BOOST_TEST(on_main);
	BOOST_TEST(all.is_ready());
}

// A shader's #include lines are dependencies, loaded by the pipeline before the shader is assembled.
BOOST_AUTO_TEST_CASE(ShaderIncludes_AreDependenciesOfTheShader)
{
	deadline_guard deadline(60);
	rs->registrate_adapter<shader_include_adapter>(shader_include_adapter::INFO);
	const auto common = put("common.glsl", "float k = 1.0;");
	const auto lighting = put("lighting.glsl", "vec3 light() { return vec3(k); }");
	const auto shader = put("main.frag", "#version 450\n#include \"common.glsl\"\n#include \"lighting.glsl\"\nvoid main() {}\n");
	const auto broken = put("broken.frag", "#version 450\n#include \"nowhere.glsl\"\n");

	gate hold(tasks, 1);
	auto shader_handle = rs->require<shader_source_resource>(shader);
	auto common_handle = rs->require<res::text_resource>(common); // queued behind the shader, which waits for it
	auto broken_handle = rs->require<shader_source_resource>(broken);
	hold.release();

	wait_finished(shader_handle);
	wait_finished(broken_handle);
	BOOST_REQUIRE(shader_handle.is_ready());
	BOOST_TEST(shader_handle.get()->str() == "#version 450\nfloat k = 1.0;\nvec3 light() { return vec3(k); }\nvoid main() {}\n");
	BOOST_TEST(common_handle.is_ready());
	BOOST_TEST(broken_handle.has_error());
	BOOST_TEST(broken_handle.get_control_block()->error_msg.find("nowhere.glsl") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

struct desc_pool_env : one_thread_env
{
	desc_pool_env() : descs(*rs)
	{
		descs.register_desc<graph_test_desc>("graph_test_desc");
		rs->registrate_adapter<core::desc::desc_adapter_t>(core::desc::desc_adapter_t::INFO, std::ref(*rs), std::ref(descs));
	}

	desc::desc_system descs;
};

BOOST_FIXTURE_TEST_SUITE(ResSystemDescDependencyTests, desc_pool_env)

BOOST_AUTO_TEST_CASE(DescWithParent_OnOneComputeThread_LoadsThroughTheDependency)
{
	deadline_guard deadline(60);
	const auto base = put("base.desc", R"({"__type":"graph_test_desc","value":5})");
	const auto mid = put("mid.desc", R"({"__type":"graph_test_desc","__parent":"memory://c/base.desc"})");
	const auto leaf = put("leaf.desc", R"({"__type":"graph_test_desc","__parent":"memory://c/mid.desc"})");

	gate hold(tasks, 1);
	auto leaf_handle = rs->require<desc::desc_base>(leaf);
	auto mid_handle = rs->require<desc::desc_base>(mid);
	auto base_handle = rs->require<desc::desc_base>(base);
	hold.release();

	wait_finished(leaf_handle);
	BOOST_REQUIRE(leaf_handle.is_ready());
	const auto loaded = std::dynamic_pointer_cast<graph_test_desc>(leaf_handle.get());
	BOOST_REQUIRE(loaded);
	BOOST_TEST(loaded->value == 5);
	BOOST_TEST(mid_handle.is_ready());
	BOOST_TEST(base_handle.is_ready());
}

BOOST_AUTO_TEST_CASE(DescParentCycleBetweenIndependentRoots_FailsBoth)
{
	deadline_guard deadline(60);
	const auto a = put("a.desc", R"({"__type":"graph_test_desc","__parent":"memory://c/b.desc"})");
	const auto b = put("b.desc", R"({"__type":"graph_test_desc","__parent":"memory://c/a.desc"})");

	gate hold(tasks, 1);
	auto a_handle = rs->require<desc::desc_base>(a);
	auto b_handle = rs->require<desc::desc_base>(b);
	hold.release();

	wait_finished(a_handle);
	wait_finished(b_handle);
	BOOST_TEST(a_handle.has_error());
	BOOST_TEST(b_handle.has_error());
	BOOST_TEST(a_handle.get_control_block()->error_msg.find("dependency cycle") != std::string::npos);
	BOOST_TEST(b_handle.get_control_block()->error_msg.find("dependency cycle") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(DescWithMissingParent_FailsWithoutHanging)
{
	deadline_guard deadline(60);
	const auto orphan = put("orphan.desc", R"({"__type":"graph_test_desc","__parent":"memory://c/nowhere.desc"})");
	auto handle = rs->require<desc::desc_base>(orphan);
	wait_finished(handle);
	BOOST_TEST(handle.has_error());
	BOOST_TEST(handle.get_control_block()->error_msg.find("nowhere.desc") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(ResSystemDependencyGraphRaceTests, two_thread_env)

// Many independent two-roots cycles at once: every pair has to be found whichever thread adds the closing edge.
BOOST_AUTO_TEST_CASE(ManyCyclesRequestedTogether_AllFailAndNothingHangs)
{
	deadline_guard deadline(60);
	constexpr int pairs = 60;
	std::vector<res::res_handle<loop_resource>> handles;
	std::vector<std::pair<res::tag, res::tag>> tags;
	for (int i = 0; i < pairs; ++i) {
		const auto name = std::to_string(i);
		const auto a = res::tag{ "memory://c/a" + name + ".loop" };
		const auto b = res::tag{ "memory://c/b" + name + ".loop" };
		put("a" + name + ".loop", b.string());
		put("b" + name + ".loop", a.string());
		tags.emplace_back(a, b);
	}
	for (const auto& [a, b] : tags) {
		handles.push_back(rs->require<loop_resource>(a));
		handles.push_back(rs->require<loop_resource>(b));
	}
	for (const auto& handle : handles) {
		wait_finished(handle);
		BOOST_TEST(handle.has_error());
		BOOST_TEST(handle.get_control_block()->error_msg.find("dependency cycle") != std::string::npos);
	}
}

BOOST_AUTO_TEST_CASE(ManyRootsSharingOneDependency_AllLoad)
{
	deadline_guard deadline(60);
	const auto parent = put("parent.txt", "parent data");
	const auto children = request_children(100, parent);
	expect_all_loaded(children);
}

BOOST_AUTO_TEST_SUITE_END()
