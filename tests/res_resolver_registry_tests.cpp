#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

// The registry of resolvers and adapters (#938): changed on the main thread while loads and the vfs threads read it.
namespace {

using namespace std::chrono_literals;

constexpr std::string_view k_protocol = "registry";

const res::tag k_tag{ "registry://a.txt" };

// What became of the resolvers made by one test: where each was destroyed, and whether a call was still in it then.
struct resolver_record
{
	std::atomic<int> alive = 0;
	std::atomic<bool> destroyed_during_call = false;
	std::thread::id destroyed_on;
};

// Answers every tag; a call can be held in it until released, to see what unmounting does meanwhile.
class held_resolver : public core::res::res_resolver_interface
{
public:
	held_resolver(std::string_view prt, std::shared_ptr<resolver_record> record) : res_resolver_interface(prt), m_record(std::move(record))
	{
		++m_record->alive;
	}

	~held_resolver() override
	{
		m_record->destroyed_during_call = m_calls.load() != 0;
		m_record->destroyed_on = std::this_thread::get_id();
		--m_record->alive;
	}

	async_raw_data resolve(const res::tag&) const override
	{
		const call_scope scope(m_calls);
		auto block = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
		block->set_ready(test_util::to_bytes("text"));
		return block;
	}

	bool exists(const res::tag&) const override
	{
		const call_scope scope(m_calls);
		std::unique_lock lock(m_mutex);
		m_entered = true;
		m_changed.notify_all();
		m_changed.wait(lock, [this] { return !m_hold; });
		return true;
	}

	// The next exists() waits in the resolver until release().
	void hold()
	{
		std::lock_guard lock(m_mutex);
		m_hold = true;
		m_entered = false;
	}

	void wait_until_entered()
	{
		std::unique_lock lock(m_mutex);
		m_changed.wait(lock, [this] { return m_entered; });
	}

	void release()
	{
		std::lock_guard lock(m_mutex);
		m_hold = false;
		m_changed.notify_all();
	}

private:
	struct call_scope
	{
		explicit call_scope(std::atomic<int>& calls) : calls(calls) { ++calls; }
		~call_scope() { --calls; }
		std::atomic<int>& calls;
	};

	std::shared_ptr<resolver_record> m_record;
	mutable std::atomic<int> m_calls = 0;
	mutable std::mutex m_mutex;
	mutable std::condition_variable m_changed;
	bool m_hold = false;
	mutable bool m_entered = false;
};

struct probe_resource : res::text_resource
{
	using res::text_resource::text_resource;
};

struct probe_adapter : core::res::adapter_interface
{
	static inline auto INFO = res::adapter_info::make<probe_resource>({ "probe" });

	std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw) const override
	{
		return std::make_shared<probe_resource>(tag, raw);
	}

	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }
};

struct registry_fixture : test_util::ram_res_env
{
	std::shared_ptr<resolver_record> record = std::make_shared<resolver_record>();
};

}

BOOST_FIXTURE_TEST_SUITE(ResResolverRegistryTests, registry_fixture)

BOOST_AUTO_TEST_CASE(Unmounting_WaitsForTheCallInTheResolver_AndDestroysItOnTheUnmountingThread)
{
	auto& mounted = rs->registrate_resolver<held_resolver>(k_protocol, record);
	mounted.hold();
	std::atomic<bool> answer = false;
	std::thread caller([&] { answer = rs->exists(k_tag); });
	mounted.wait_until_entered();

	std::thread releaser([&] {
		std::this_thread::sleep_for(50ms);
		mounted.release();
	});
	rs->unregistrate_resolver(k_protocol);
	caller.join();
	releaser.join();

	BOOST_TEST(record->alive.load() == 0);
	BOOST_TEST(!record->destroyed_during_call.load(), "destroyed only after the call in it ended");
	BOOST_TEST((record->destroyed_on == std::this_thread::get_id()), "its threads are joined by the thread that unmounts");
	BOOST_TEST(answer.load(), "the call ended against a live resolver");
	BOOST_TEST(!rs->exists(k_tag), "not mounted any more");
}

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

constexpr int k_readers = 4;
constexpr int k_rounds = 200;
// Every so many rounds the main thread waits until the readers got through again: they ran during the rounds.
constexpr int k_rounds_per_check = 20;

BOOST_AUTO_TEST_CASE(RemountingWhileOtherThreadsRead_NeverReadsAGoneResolver)
{
	rs->registrate_resolver<held_resolver>(k_protocol, record);
	std::atomic<bool> stop = false;
	std::atomic<int> started = 0;
	std::atomic<int> answered = 0;
	std::vector<std::thread> readers;
	for (int i = 0; i < k_readers; ++i) {
		readers.emplace_back([&] {
			bool counted = false;
			while (!stop) {
				if (rs->fetch_data(k_tag)) {
					++answered;
					if (!counted) {
						counted = true;
						++started;
					}
				}
				std::this_thread::yield();
			}
		});
	}
	BOOST_REQUIRE(wait_past(started, k_readers - 1));
	for (int round = 0; round < k_rounds; ++round) {
		rs->unregistrate_resolver(k_protocol);
		rs->registrate_resolver<held_resolver>(k_protocol, record);
		if (round % k_rounds_per_check == 0) {
			BOOST_REQUIRE(wait_past(answered, answered.load()));
		}
	}
	stop = true;
	for (auto& reader : readers) reader.join();

	BOOST_TEST(record->alive.load() == 1, "every unmounted resolver is gone, the last one is mounted");
	BOOST_TEST(!record->destroyed_during_call.load());
}

BOOST_AUTO_TEST_CASE(AddingAndRemovingAnAdapterWhileOtherThreadsLookItUp)
{
	std::atomic<bool> stop = false;
	std::atomic<int> started = 0;
	std::atomic<int> found = 0;
	std::vector<std::thread> readers;
	for (int i = 0; i < k_readers; ++i) {
		readers.emplace_back([&] {
			bool counted = false;
			while (!stop) {
				if (!rs->registered_extensions(probe_adapter::INFO).empty()) ++found;
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
		rs->registrate_adapter<probe_adapter>(probe_adapter::INFO);
		if (round % k_rounds_per_check == 0) {
			BOOST_REQUIRE(wait_past(found, found.load()));
		}
		rs->unregistrate_adapter(probe_adapter::INFO);
	}
	stop = true;
	for (auto& reader : readers) reader.join();

	BOOST_TEST(found.load() > 0);
	BOOST_TEST(rs->registered_extensions(probe_adapter::INFO).empty());
}

BOOST_AUTO_TEST_SUITE_END()
