#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
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

// The reader threads of a test: stopped and joined however the test ends, a failed REQUIRE included (a joinable
// std::thread destroyed would terminate the whole run).
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

constexpr int k_readers = 4;
constexpr int k_rounds = 200;
// Every so many rounds the main thread waits until the readers got through again: they ran during the rounds.
constexpr int k_rounds_per_check = 20;

BOOST_AUTO_TEST_CASE(RemountingWhileOtherThreadsRead_NeverReadsAGoneResolver)
{
	rs->registrate_resolver<held_resolver>(k_protocol, record);
	std::atomic<int> started = 0;
	std::atomic<int> answered = 0;
	reader_threads readers;
	for (int i = 0; i < k_readers; ++i) {
		readers.threads.emplace_back([&] {
			bool counted = false;
			while (!readers.stop) {
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
	readers.join();

	BOOST_TEST(record->alive.load() == 1, "every unmounted resolver is gone, the last one is mounted");
	BOOST_TEST(!record->destroyed_during_call.load());
}

BOOST_AUTO_TEST_CASE(AddingAndRemovingAnAdapterWhileOtherThreadsLookItUp)
{
	std::atomic<int> started = 0;
	std::atomic<int> found = 0;
	reader_threads readers;
	for (int i = 0; i < k_readers; ++i) {
		readers.threads.emplace_back([&] {
			bool counted = false;
			while (!readers.stop) {
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
	readers.join();

	BOOST_TEST(found.load() > 0);
	BOOST_TEST(rs->registered_extensions(probe_adapter::INFO).empty());
}

// #1034: the second mount of a protocol either replaces the first (its resolver gone) or is refused.
namespace {

constexpr std::string_view k_mounted = "mount1034";

std::filesystem::path folder_with(const std::string& name, const std::string& file)
{
	const auto folder = std::filesystem::temp_directory_path() / "res_mount_1034" / name;
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder);
	std::ofstream(folder / file) << "text";
	return folder;
}

res::mount mount_of(const std::filesystem::path& folder)
{
	res::mount made;
	made.protocol = k_mounted;
	made.roots = { folder };
	made.access = res::mount_access::read_only;
	return made;
}

}

BOOST_AUTO_TEST_CASE(AMountAfterItsResolverWasUnregistered_ReplacesTheMountsRoots)
{
	const auto first = folder_with("first", "a.txt");
	const auto second = folder_with("second", "b.txt");
	BOOST_REQUIRE(rs->add_mount(mount_of(first)));
	rs->unregistrate_resolver(k_mounted);
	BOOST_TEST((rs->mount_roots(k_mounted) == std::vector{ first }));

	BOOST_REQUIRE(rs->add_mount(mount_of(second)));
	BOOST_TEST((rs->mount_roots(k_mounted) == std::vector{ second }));
	BOOST_TEST(rs->exists(res::tag{ "mount1034://b.txt" }));
	BOOST_TEST(!rs->exists(res::tag{ "mount1034://a.txt" }));
	rs->unregistrate_resolver(k_mounted);
}

BOOST_AUTO_TEST_CASE(AMountOfAProtocolThatIsServed_IsRefused_AndChangesNothing)
{
	const auto first = folder_with("served", "a.txt");
	const auto second = folder_with("refused", "b.txt");
	BOOST_REQUIRE(rs->add_mount(mount_of(first)));

	BOOST_TEST(!rs->add_mount(mount_of(second)));
	BOOST_TEST((rs->mount_roots(k_mounted) == std::vector{ first }));
	BOOST_TEST(rs->exists(res::tag{ "mount1034://a.txt" }));
	BOOST_TEST(!rs->exists(res::tag{ "mount1034://b.txt" }));
	rs->unregistrate_resolver(k_mounted);
}

// #996: the file a tag is read from, while it holds the bytes a load got; none from a resolver that reads no files.
BOOST_AUTO_TEST_CASE(TheDiskFileOfATag_IsTheOneItsResolverReads_WhileItHoldsTheBytes)
{
	const auto low = folder_with("disk_low", "a.txt");
	const auto high = folder_with("disk_high", "a.txt");
	std::ofstream(high / "a.txt") << "high";
	res::mount both = mount_of(low);
	both.roots.push_back(high);
	BOOST_REQUIRE(rs->add_mount(std::move(both)));
	const res::tag tag{ "mount1034://a.txt" };

	std::string differs;
	const auto found = rs->disk_file_holding(tag, test_util::to_bytes("high"), differs);
	BOOST_REQUIRE(found.has_value());
	BOOST_TEST(std::filesystem::equivalent(found->root, high));
	BOOST_TEST(std::filesystem::equivalent(found->file, high / "a.txt"));
	BOOST_TEST(differs.empty());

	BOOST_TEST(!rs->disk_file_holding(tag, test_util::to_bytes("text"), differs).has_value());
	BOOST_TEST(!differs.empty());
	differs.clear();
	BOOST_TEST(!rs->disk_file_holding(res::tag{ "mount1034://none.txt" }, test_util::to_bytes("text"), differs));
	BOOST_TEST(differs.empty());

	rs->unregistrate_resolver(k_mounted);
	rs->registrate_resolver<test_util::ram_resolver>(k_mounted);
	BOOST_TEST(!rs->disk_file_holding(tag, test_util::to_bytes("high"), differs).has_value());
	BOOST_TEST(differs.empty());
	rs->unregistrate_resolver(k_mounted);
}

// #1086: what the resolver of a tag opens for it now, reads past the cache, and lists below a folder.
BOOST_AUTO_TEST_CASE(TheFileOfATag_ItsBytesAndAFoldersFiles_AreTheResolvers)
{
	const auto low = folder_with("asked_low", "a.txt");
	const auto high = folder_with("asked_high", "a.txt");
	std::ofstream(high / "a.txt") << "high";
	std::filesystem::create_directories(low / "dir" / "sub");
	std::ofstream(low / "dir" / "sub" / "b.txt") << "b";
	std::filesystem::create_directories(high / "dir");
	std::ofstream(high / "dir" / "c.txt") << "c";
	res::mount both = mount_of(low);
	both.roots.push_back(high);
	BOOST_REQUIRE(rs->add_mount(std::move(both)));
	using platform::beneath::read_status;
	const res::tag a_txt{ "mount1034://a.txt" };
	const res::tag none_txt{ "mount1034://none.txt" };

	const auto opened = rs->disk_file_of(a_txt);
	BOOST_REQUIRE(opened.has_value());
	BOOST_TEST(std::filesystem::equivalent(opened->root, high));
	// A missing file is opened in the top layer, where a store would put it.
	const auto missing = rs->disk_file_of(none_txt);
	BOOST_REQUIRE(missing.has_value());
	BOOST_TEST(std::filesystem::equivalent(missing->root, high));

	test_util::in_task(tasks, [&] {
		std::vector<std::byte> bytes;
		BOOST_TEST((rs->read_now(a_txt, 16, bytes).status == read_status::ok));
		BOOST_TEST((bytes == test_util::to_bytes("high")));
		BOOST_TEST((rs->read_now(a_txt, 3, bytes).status == read_status::too_large));
		BOOST_TEST((rs->read_now(none_txt, 16, bytes).status != read_status::ok));
	});

	const auto listed = rs->files_under(k_mounted, "dir/");
	BOOST_REQUIRE(listed.has_value());
	const std::vector<res::tag> expected{ res::tag{ "mount1034://dir/c.txt" }, res::tag{ "mount1034://dir/sub/b.txt" } };
	BOOST_TEST((*listed == expected));
	BOOST_TEST(rs->files_under(k_mounted, "nothing/").value().empty());

	rs->unregistrate_resolver(k_mounted);
	rs->registrate_resolver<test_util::ram_resolver>(k_mounted).files[a_txt] = test_util::to_bytes("ram");
	BOOST_TEST(!rs->disk_file_of(a_txt).has_value());
	BOOST_TEST(!rs->files_under(k_mounted, "dir/").has_value());
	test_util::in_task(tasks, [&] {
		std::vector<std::byte> bytes;
		BOOST_TEST((rs->read_now(a_txt, 16, bytes).status == read_status::ok));
		BOOST_TEST((bytes == test_util::to_bytes("ram")));
	});
	rs->unregistrate_resolver(k_mounted);
}

// #1086: a link in a listed folder is not followed, as the resolver would not read through it either.
BOOST_AUTO_TEST_CASE(AFoldersFiles_LeaveOutLinks)
{
	const auto folder = folder_with("linked", "a.txt");
	const auto outside = folder_with("outside", "secret.txt");
	std::filesystem::create_directories(folder / "dir");
	std::ofstream(folder / "dir" / "kept.txt") << "k";
	std::error_code ec;
	std::filesystem::create_symlink(outside / "secret.txt", folder / "dir" / "file_link.txt", ec);
	const bool file_linked = !ec;
	std::filesystem::create_directory_symlink(outside, folder / "dir" / "dir_link", ec);
	const bool dir_linked = !ec;
	if (!file_linked && !dir_linked) BOOST_TEST_MESSAGE("links cannot be made here: only the plain file is checked");
	BOOST_REQUIRE(rs->add_mount(mount_of(folder)));

	const auto listed = rs->files_under(k_mounted, "dir");
	BOOST_REQUIRE(listed.has_value());
	BOOST_TEST((*listed == std::vector<res::tag>{ res::tag{ "mount1034://dir/kept.txt" } }));
	rs->unregistrate_resolver(k_mounted);
}

BOOST_AUTO_TEST_SUITE_END()
