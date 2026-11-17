#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/res_system.h>
#include <snk/model_codec/res_tag.h>
#include "test_manual_tasks.hpp"

namespace json = boost::json;

// The registries of desc_system (#988): changed on the test thread while other threads migrate and create descs.
namespace {

using namespace std::chrono_literals;

constexpr std::string_view k_type = "registry_lock_desc";
constexpr std::string_view k_other_type = "registry_lock_other";
constexpr std::string_view k_transient_type = "registry_lock_transient";
constexpr std::string_view k_held_type = "registry_lock_held";

// A call that waits in a step or a factory until the test opens the gate; 10 s at most, so a failure cannot hang CI.
class gate
{
public:
	// The held call: true when it was let go, false when it timed out.
	bool enter()
	{
		std::unique_lock lock(m_mutex);
		m_entered = true;
		m_changed.notify_all();
		return m_changed.wait_for(lock, 10s, [this] { return m_open; });
	}

	bool wait_entered()
	{
		std::unique_lock lock(m_mutex);
		return m_changed.wait_for(lock, 10s, [this] { return m_entered; });
	}

	void open()
	{
		std::lock_guard lock(m_mutex);
		m_open = true;
		m_changed.notify_all();
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_changed;
	bool m_entered = false;
	bool m_open = false;
};

struct lock_test_desc : desc::desc_base
{
	int value = 0;

	void deserialize(desc::desc_system&, const json::object& obj) override
	{
		if (const json::value* found = obj.if_contains("value"); found && found->is_int64()) value = static_cast<int>(found->as_int64());
	}
	void serialize(json::object& obj) const override { obj["value"] = value; }
};

// The gate held_desc's constructor waits in, i.e. the factory of k_held_type.
std::atomic<gate*> g_factory_gate = nullptr;

struct held_desc : lock_test_desc
{
	held_desc()
	{
		if (gate* held = g_factory_gate.load()) {
			opened = held->enter();
		}
	}

	bool opened = false;
};

struct factory_gate_scope
{
	explicit factory_gate_scope(gate& held) { g_factory_gate = &held; }
	~factory_gate_scope() { g_factory_gate = nullptr; }
	factory_gate_scope(const factory_gate_scope&) = delete;
	factory_gate_scope& operator=(const factory_gate_scope&) = delete;
};

// Waits until `counter` is past `above`: false after 10 s, a reader that stopped.
bool wait_past(const std::atomic<int>& counter, int above)
{
	const auto deadline = std::chrono::steady_clock::now() + 10s;
	while (counter.load() <= above) {
		if (std::chrono::steady_clock::now() > deadline) return false;
		std::this_thread::sleep_for(1ms);
	}
	return true;
}

// The threads of a test: stopped and joined however the test ends, a failed REQUIRE included (a joinable std::thread
// destroyed would terminate the whole run).
struct reader_threads
{
	~reader_threads() { join(); }

	void join()
	{
		stop = true;
		for (auto& thread : threads) {
			if (thread.joinable()) thread.join();
		}
	}

	std::atomic<bool> stop = false;
	std::vector<std::thread> threads;
};

// resource_system asserts that its resources directory exists; base classes are built before members.
struct res_dir_guard
{
	res_dir_guard() { std::filesystem::create_directories("./res"); }
};

struct registry_fixture : res_dir_guard
{
	registry_fixture() : descs(rs) {}

	ds::task_system tasks{ test_util::manual_tasks() };
	res::resource_system rs{ tasks };
	desc::desc_system descs;
};

// Migrated objects either went through the one step of their type or were left at version 1.
bool migrated_or_left(const json::object& obj, std::string_view mark)
{
	const json::value* version = obj.if_contains("__version");
	if (!version || !version->is_int64()) return false;
	return (version->as_int64() == 1 && !obj.contains(mark)) || (version->as_int64() == 2 && obj.contains(mark));
}

constexpr int k_readers = 4;
constexpr int k_rounds = 200;
// Every so many rounds the test thread waits until the readers got through again: they ran during the rounds.
constexpr int k_rounds_per_check = 20;

}

BOOST_FIXTURE_TEST_SUITE(DescRegistryConcurrencyTests, registry_fixture)

BOOST_AUTO_TEST_CASE(RegisteringAndUnregisteringWhileOtherThreadsMigrate_EveryResultIsOneOfTheValidOnes)
{
	descs.register_desc<lock_test_desc>(k_type);
	const res::tag tag{ "memory://t/reader.desc" };
	std::atomic<int> started = 0;
	std::atomic<int> passes = 0;
	std::atomic<int> bad = 0;
	reader_threads readers;
	for (int i = 0; i < k_readers; ++i) {
		readers.threads.emplace_back([&] {
			bool counted = false;
			while (!readers.stop) {
				try {
					json::object obj{ { "__type", k_type }, { "child", json::object{ { "__type", k_other_type } } } };
					descs.migrate_tree(obj, "reader");
					const json::value* child = obj.if_contains("child");
					bool ok = child && child->is_object() && migrated_or_left(obj, "m1") && migrated_or_left(child->as_object(), "y1");
					const int current = descs.current_version(k_type);
					ok = ok && (current == 1 || current == 2);
					const auto made = descs.create_instance(k_type, tag);
					ok = ok && made && made->get_type() == k_type;
					static_cast<void>(descs.is_registered(k_transient_type));
					if (!ok) ++bad;
				} catch (const std::exception&) {
					++bad;
				}
				++passes;
				if (!counted) {
					counted = true;
					++started;
				}
				std::this_thread::yield();
			}
		});
	}
	BOOST_REQUIRE(wait_past(started, k_readers - 1));
	for (int round = 0; round < k_rounds; ++round) {
		descs.register_migration(k_type, 1, [](json::object& obj) { obj["m1"] = true; });
		descs.register_migration(k_other_type, 1, [](json::object& obj) { obj["y1"] = true; });
		descs.register_desc<lock_test_desc>(k_transient_type);
		descs.unregister_migrations(k_type);
		descs.unregister_migrations(k_other_type);
		descs.unregister_desc(k_transient_type);
		if (round % k_rounds_per_check == 0) {
			BOOST_REQUIRE(wait_past(passes, passes.load()));
		}
	}
	readers.join();

	BOOST_TEST(bad.load() == 0, "a reader saw a result that is neither migrated by the step nor left at version 1");
	BOOST_TEST(descs.current_version(k_type) == 1);
	BOOST_TEST(!descs.is_registered(k_transient_type));
}

BOOST_AUTO_TEST_CASE(RunningStep_SurvivesUnregisteringItsMigrations)
{
	gate step_gate;
	auto token = std::make_shared<int>(0);
	const std::weak_ptr<int> token_watch = token;
	std::atomic<bool> opened = false;
	descs.register_migration(k_type, 1, [captured = std::move(token), &step_gate, &opened](json::object& obj) {
		opened = step_gate.enter();
		obj["m1"] = *captured + 1;
	});

	json::object obj{ { "__type", k_type } };
	std::atomic<bool> failed = false;
	reader_threads worker;
	worker.threads.emplace_back([&] {
		try {
			descs.migrate_tree(obj, "worker");
		} catch (const std::exception&) {
			failed = true;
		}
	});
	BOOST_REQUIRE(step_gate.wait_entered());

	descs.unregister_migrations(k_type);
	BOOST_TEST(descs.current_version(k_type) == 1);
	BOOST_TEST(!token_watch.expired(), "the running step keeps what it captured");
	step_gate.open();
	worker.join();

	BOOST_TEST(opened.load(), "unregister_migrations returned while the step was still waiting");
	BOOST_TEST(!failed.load());
	BOOST_TEST(obj.at("m1").as_int64() == 1);
	BOOST_TEST(obj.at("__version").as_int64() == 2);
	BOOST_TEST(token_watch.expired(), "released with the last copy of the steps");
}

// The factory of add_desc<T> holds no state, so the copy taken under the lock is not what this case proves (the step case does).
BOOST_AUTO_TEST_CASE(AFactoryCallInFlight_ReturnsAValidInstance_WhenItsDescIsUnregistered)
{
	gate factory_gate;
	const factory_gate_scope scope(factory_gate);
	descs.register_desc<held_desc>(k_held_type);

	std::shared_ptr<desc::desc_base> made;
	reader_threads worker;
	worker.threads.emplace_back([&] { made = descs.create_instance(k_held_type, res::tag{ "memory://t/held.desc" }); });
	BOOST_REQUIRE(factory_gate.wait_entered());

	descs.unregister_desc(k_held_type);
	BOOST_TEST(!descs.is_registered(k_held_type));
	factory_gate.open();
	worker.join();

	BOOST_REQUIRE(made);
	BOOST_TEST((made->get_type() == k_held_type));
	const auto held = std::dynamic_pointer_cast<held_desc>(made);
	BOOST_REQUIRE(held);
	BOOST_TEST(held->opened, "unregister_desc returned while the factory was still waiting");
}

BOOST_AUTO_TEST_CASE(RegisterUnregisterRegister_OnOneThread_BehavesAsAFreshRegistration)
{
	const auto* stored = descs.register_schema(desc::parse_type_schema(json::parse(R"({ "name": "registry_lock_desc", "version": 1, "fields": [] })").as_object()));
	BOOST_REQUIRE(stored);

	descs.register_migration(k_type, 1, [](json::object& obj) { obj["first"] = true; });
	descs.register_migration(k_type, 2, "the second step", [](json::object& obj, desc::migration_context&) { obj["second"] = true; });
	BOOST_TEST(descs.current_version(k_type) == 3);
	BOOST_TEST(stored->version == 3);
	BOOST_TEST(descs.migration_summary(k_type, 2) == "the second step");

	descs.unregister_migrations(k_type);
	BOOST_TEST(descs.current_version(k_type) == 1);
	BOOST_TEST(stored->version == 1);
	BOOST_TEST(descs.migration_summary(k_type, 2).empty());
	json::object unmigrated{ { "__type", k_type } };
	descs.migrate_tree(unmigrated, "ctx");
	BOOST_TEST(unmigrated.at("__version").as_int64() == 1);
	BOOST_TEST(!unmigrated.contains("first"));

	descs.register_migration(k_type, 1, [](json::object& obj) { obj["again"] = true; });
	BOOST_TEST(descs.current_version(k_type) == 2);
	BOOST_TEST(stored->version == 2);
	BOOST_TEST(descs.migration_summary(k_type, 1).empty());
	json::object migrated{ { "__type", k_type } };
	descs.migrate_tree(migrated, "ctx");
	BOOST_TEST(migrated.at("__version").as_int64() == 2);
	BOOST_TEST(migrated.contains("again"));
	BOOST_TEST(!migrated.contains("first"));
	BOOST_TEST(!migrated.contains("second"));

	json::object newer{ { "__type", k_type }, { "__version", 3 } };
	BOOST_CHECK_THROW(descs.migrate_tree(newer, "ctx"), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
