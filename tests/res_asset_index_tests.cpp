#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include <snk/config/cfg_variables.h>
#include <snk/resource/res_asset_index.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_binary.h>
#include <snk/resource/resources/res_resource_text.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// The runtime key of an asset is its ID (EPIC-57 section 5, S57-7): resource_system with an asset index.

namespace {

namespace fs = std::filesystem;

// What the editor's asset database answers: one ID per file, any number of spellings for it.
class fake_index : public res::asset_index
{
public:
	std::optional<res::asset_id> id_of(const res::tag& tag) const override
	{
		std::lock_guard lock(m_mutex);
		const auto it = m_ids.find(tag);
		return it == m_ids.end() ? std::nullopt : std::optional{ it->second };
	}

	std::optional<res::tag> path_of(const res::asset_id& id) const override
	{
		std::lock_guard lock(m_mutex);
		const auto it = m_paths.find(id);
		return it == m_paths.end() ? std::nullopt : std::optional{ it->second };
	}

	void put(const res::tag& tag, const res::asset_id& id)
	{
		std::lock_guard lock(m_mutex);
		m_ids[tag] = id;
		m_paths[id] = tag;
	}

	void spell(const res::tag& spelling, const res::asset_id& id)
	{
		std::lock_guard lock(m_mutex);
		m_ids[spelling] = id;
	}

	void move(const res::tag& from, const res::tag& to)
	{
		std::lock_guard lock(m_mutex);
		const res::asset_id id = m_ids.at(from);
		m_ids.erase(from);
		m_ids[to] = id;
		m_paths[id] = to;
	}

	void remove(const res::tag& tag)
	{
		std::lock_guard lock(m_mutex);
		m_paths.erase(m_ids.at(tag));
		m_ids.erase(tag);
	}

private:
	mutable std::mutex m_mutex;
	std::unordered_map<res::tag, res::asset_id> m_ids;
	std::unordered_map<res::asset_id, res::tag> m_paths;
};

res::asset_id id_number(std::uint64_t n)
{
	return res::asset_id{ 0x6f1c0b5200004000ull, 0x8000000000000000ull + n };
}

const res::asset_id k_a = id_number(1);
const res::asset_id k_b = id_number(2);

// A file that is there and does not load.
class broken_resolver : public test_util::ram_resolver
{
public:
	using ram_resolver::ram_resolver;

	async_raw_data resolve(const res::tag& tag) const override
	{
		auto block = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
		block->set_error("broken file: " + tag.string());
		return block;
	}

	bool exists(const res::tag&) const override { return true; }
};

using moves = std::vector<std::pair<res::tag, res::tag>>;

// What a watcher was told, by tag.
struct watch_log
{
	std::vector<std::string> told;

	void watch(res::resource_system& rs, const res::tag& tag)
	{
		rs.watch(tag, this, [this](const res::tag& changed) { told.emplace_back(changed.view()); });
	}
};

std::vector<std::string> names(std::initializer_list<res::tag> tags)
{
	std::vector<std::string> out;
	for (const auto& tag : tags) {
		out.emplace_back(tag.view());
	}
	return out;
}

// memory:// in RAM: what the index binds, without files that go away (no batches).
struct index_fixture : test_util::ram_res_env
{
	index_fixture()
	{
		rs->set_asset_index(index);
	}

	std::string text(const res::tag& tag)
	{
		const auto ptr = rs->require_sync<res::text_resource>(tag).get_sync();
		return ptr ? ptr->str() : std::string{ "<failed>" };
	}

	std::shared_ptr<fake_index> index = std::make_shared<fake_index>();
};

// res:// on disk that reports its changes and allows remove/rename: the batches the index owner takes in.
struct disk_fixture
{
	test_util::temp_dir dir{ "res_asset_index_disk" };
	ds::task_system tasks{ test_util::manual_tasks() };
	std::shared_ptr<fake_index> index = std::make_shared<fake_index>();
	std::unique_ptr<res::resource_system> rs;

	disk_fixture()
	{
		dir.write("res/a.txt", "A");
		dir.write("res/c.txt", "C");
		res::watch_options watch;
		watch.report_changes = true;
		rs = std::make_unique<res::resource_system>(tasks, res::mount_table{
			res::mount{ res::tag::default_protocol(), { dir.path() / "res" }, res::mount_access::read_write,
				res::write_policy::allow_all(), std::move(watch), true },
		});
		rs->set_asset_index(index);
	}

	~disk_fixture()
	{
		rs.reset();
	}

	static res::tag at(std::string_view name)
	{
		std::string path{ "res://" };
		path.append(name);
		return res::tag{ std::move(path) };
	}

	void pump() { test_util::pump(tasks); }

	// The index owner took in every batch delivered so far.
	void apply(const moves& moved)
	{
		rs->apply_asset_batch(rs->delivering_batch(), moved);
		pump();
	}
};

// res:// on disk that allows remove/rename with no file watcher at all: no batch, no poll, the test drives every change.
struct quiet_disk_fixture
{
	struct no_file_watching
	{
		no_file_watching() { cfg::set_variable("res/watch_files", "false"); }
		~no_file_watching() { cfg::set_variable("res/watch_files", "true"); }
	};

	no_file_watching unwatched;
	test_util::temp_dir dir{ "res_asset_index_quiet_disk" };
	ds::task_system tasks{ test_util::manual_tasks() };
	std::shared_ptr<fake_index> index = std::make_shared<fake_index>();
	std::unique_ptr<res::resource_system> rs;

	quiet_disk_fixture()
	{
		dir.write("res/a.txt", "A");
		dir.write("res/c.txt", "C");
		rs = std::make_unique<res::resource_system>(tasks, res::mount_table{
			res::mount{ res::tag::default_protocol(), { dir.path() / "res" }, res::mount_access::read_write,
				res::write_policy::allow_all(), res::watch_options{}, true },
		});
		rs->set_asset_index(index);
	}

	~quiet_disk_fixture()
	{
		rs.reset();
	}

	void pump() { test_util::pump(tasks); }
};

}

BOOST_AUTO_TEST_SUITE(ResAssetIndexTests)

BOOST_FIXTURE_TEST_CASE(TwoTagsOfOneId_ShareOneBlock, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag spelled = tag_of("A.txt");
	index->put(a, k_a);
	index->spell(spelled, k_a);

	const auto first = rs->require_sync<res::text_resource>(a);
	const auto second = rs->require_sync<res::text_resource>(spelled);

	BOOST_TEST(first.get_control_block() == second.get_control_block());
	BOOST_TEST(second.get_sync()->str() == "A");
	// The handle shows what was asked for.
	BOOST_TEST(second.get_tag() == spelled);
}

BOOST_FIXTURE_TEST_CASE(DuplicateCopyTheIndexLeavesOut_LoadsItsOwnContent, index_fixture)
{
	// A copy made together with its .resource: the index answers for the original only.
	const res::tag original = put("a.txt", "original");
	const res::tag copy = put("copy.txt", "copy");
	index->put(original, k_a);

	const auto first = rs->require_sync<res::text_resource>(original);
	const auto second = rs->require_sync<res::text_resource>(copy);

	BOOST_TEST(first.get_control_block() != second.get_control_block());
	BOOST_TEST(text(original) == "original");
	BOOST_TEST(text(copy) == "copy");
}

BOOST_FIXTURE_TEST_CASE(ContentChangeOfAFileWithId_EvictsAtOnce, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	index->put(a, k_a);
	const auto held = rs->require_sync<res::text_resource>(a);

	ram->files[a] = test_util::to_bytes("A2");
	rs->signal_changed(a);
	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(rs->require_sync<res::text_resource>(a).get_control_block() != held.get_control_block());
	BOOST_TEST(text(a) == "A2");
}

BOOST_FIXTURE_TEST_CASE(ResolveReference_FollowsTheIdToItsFile, index_fixture)
{
	const res::tag b = put("b.txt", "B");
	index->put(b, k_a);

	BOOST_TEST(rs->resolve_reference(res::reference{ k_a, tag_of("old.txt") }) == b);
	BOOST_TEST(rs->resolve_reference(res::reference{ k_a, res::tag{} }) == b);
	BOOST_TEST(rs->resolve_reference(res::reference{ k_b, tag_of("old.txt") }) == tag_of("old.txt"));
	BOOST_TEST(!rs->resolve_reference(res::reference{ k_b, res::tag{} }).is_valid());
}

BOOST_FIXTURE_TEST_CASE(WithoutIndex_EverythingIsKeyedByTag, test_util::ram_res_env)
{
	const res::tag a = put("a.txt", "A");
	const auto held = rs->require_sync<res::text_resource>(a);
	watch_log log;
	log.watch(*rs, a);

	ram->files.erase(a);
	rs->signal_changed(a);
	pump();

	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(log.told == names({ a }));
	BOOST_TEST(rs->resolve_reference(res::reference{ k_a, tag_of("p.txt") }) == tag_of("p.txt"));
	rs->unwatch(a, &log);
}

BOOST_FIXTURE_TEST_CASE(GoneWithoutABatch_IsEvictedWhenTheIndexIsDetached, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	index->put(a, k_a);
	const auto held = rs->require_sync<res::text_resource>(a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned")));
	watch_log log;
	log.watch(*rs, a);

	ram->files.erase(a);
	rs->signal_changed(a);
	pump();
	BOOST_TEST(rs->waiting_for_index_count() == 1u);
	BOOST_TEST(log.told.empty());

	rs->set_asset_index(nullptr);
	pump();
	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(log.told == names({ a }));

	// Detached, what was keyed by the ID is keyed by the tag: signals reach the watcher, unpin finds the pin.
	rs->signal_changed(a);
	pump();
	BOOST_TEST(log.told == names({ a, a }));
	BOOST_TEST(rs->pinned_count() == 1u);
	rs->unpin_resource(a);
	BOOST_TEST(rs->pinned_count() == 0u);
	rs->unwatch(a, &log);
	BOOST_TEST(rs->watched_tag_count() == 0u);
}

BOOST_FIXTURE_TEST_CASE(PinnedBeforeTheFileHadAnId_IsFoundByItsTagAfterwards, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned")));
	watch_log log;
	log.watch(*rs, a);

	// The file gets its .resource: the index owner's batch binds the tag to the ID, the entries move with it.
	index->put(a, k_a);
	rs->apply_asset_batch(rs->delivering_batch(), {});

	BOOST_TEST(text(a) == "pinned");
	rs->signal_changed(a);
	pump();
	BOOST_TEST(log.told == names({ a }));
	BOOST_TEST(rs->pinned_count() == 1u);
	rs->unpin_resource(a);
	BOOST_TEST(rs->pinned_count() == 0u);
	rs->unwatch(a, &log);
	BOOST_TEST(rs->watched_tag_count() == 0u);
}

BOOST_FIXTURE_TEST_CASE(AnotherIndex_MovesTheEntriesToItsKeys, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	index->put(a, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned")));
	watch_log log;
	log.watch(*rs, a);

	auto other = std::make_shared<fake_index>();
	other->put(a, k_b);
	rs->set_asset_index(other);

	BOOST_TEST(text(a) == "pinned");
	rs->signal_changed(a);
	pump();
	BOOST_TEST(log.told == names({ a }));
	rs->unpin_resource(a);
	BOOST_TEST(rs->pinned_count() == 0u);
	rs->unwatch(a, &log);
	BOOST_TEST(rs->watched_tag_count() == 0u);
}

BOOST_FIXTURE_TEST_CASE(RequestsAndSignalsFromThreads_WhileBatchesApplyAndTheIndexChanges, index_fixture)
{
	constexpr int k_files = 4;
	std::vector<res::tag> tags;
	auto other = std::make_shared<fake_index>();
	for (int i = 0; i < k_files; ++i) {
		tags.push_back(put("f" + std::to_string(i) + ".txt", std::to_string(i)));
		index->put(tags.back(), id_number(10 + i));
		other->put(tags.back(), id_number(20 + i));
	}

	constexpr int k_requesters = 3;
	constexpr int k_requests = 300;
	std::atomic<bool> go{ false };
	std::vector<std::vector<std::pair<int, res::res_handle<res::text_resource>>>> handles(k_requesters);
	std::vector<std::thread> threads;
	for (int t = 0; t < k_requesters; ++t) {
		threads.emplace_back([&, t] {
			while (!go.load()) {
				std::this_thread::yield();
			}
			for (int i = 0; i < k_requests; ++i) {
				const int file = (t + i) % k_files;
				handles[t].emplace_back(file, rs->require<res::text_resource>(tags[file]));
			}
		});
	}
	threads.emplace_back([&] {
		while (!go.load()) {
			std::this_thread::yield();
		}
		for (int i = 0; i < k_requests; ++i) {
			rs->signal_changed(tags[i % k_files]);
		}
	});

	go = true;
	for (int i = 0; i < 40; ++i) {
		if (i == 20) {
			rs->set_asset_index(other);
		}
		rs->apply_asset_batch(rs->delivering_batch(), {});
	}
	for (auto& thread : threads) {
		thread.join();
	}
	pump();

	for (const auto& per_thread : handles) {
		for (const auto& [file, handle] : per_thread) {
			BOOST_REQUIRE(handle.is_ready());
			BOOST_TEST(handle.get_sync()->str() == std::to_string(file));
		}
	}
	BOOST_TEST(rs->waiting_for_index_count() == 0u);
}

BOOST_FIXTURE_TEST_CASE(Rename_ThroughResourceSystem_KeepsTheLoadedPinnedAndWatched, disk_fixture)
{
	const res::tag a = at("a.txt");
	const res::tag b = at("b.txt");
	index->put(a, k_a);
	const auto before = rs->require_sync<res::text_resource>(a);
	watch_log log;
	log.watch(*rs, a);
	{
		res::resource_system layer(*rs);
		layer.pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned")));
		const auto pinned = layer.require_sync<res::text_resource>(a).get_control_block();

		BOOST_REQUIRE(rs->rename(a, b));
		// Nothing is forgotten before the batch says whether the file moved.
		BOOST_TEST(rs->waiting_for_index_count() == 1u);
		BOOST_TEST(layer.pinned_count() == 1u);
		pump();
		index->move(a, b);
		apply({ { a, b } });

		BOOST_TEST(rs->waiting_for_index_count() == 0u);
		BOOST_TEST(log.told.empty());
		BOOST_TEST(layer.pinned_count() == 1u);
		BOOST_TEST(rs->require_sync<res::text_resource>(b).get_control_block() == before.get_control_block());
		BOOST_TEST(layer.require_sync<res::text_resource>(b).get_control_block() == pinned);
		layer.unpin_resource(b);
		BOOST_TEST(layer.pinned_count() == 0u);
	}

	// The watcher follows the file: told about b, found by the tag it watched.
	rs->signal_changed(b);
	pump();
	BOOST_TEST(log.told == names({ b }));
	rs->unwatch(a, &log);
	BOOST_TEST(rs->watched_tag_count() == 0u);
}

BOOST_FIXTURE_TEST_CASE(Remove_ThroughResourceSystem_IsForgottenOnceTheBatchCame, disk_fixture)
{
	const res::tag a = at("a.txt");
	index->put(a, k_a);
	const auto held = rs->require_sync<res::text_resource>(a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned")));
	watch_log log;
	log.watch(*rs, a);

	BOOST_REQUIRE(rs->remove(a));
	BOOST_TEST(rs->waiting_for_index_count() == 1u);
	BOOST_TEST(rs->pinned_count() == 1u);
	pump();
	BOOST_TEST(log.told.empty());

	index->remove(a);
	apply({});
	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(rs->pinned_count() == 0u);
	BOOST_TEST(log.told == names({ a }));
	rs->unwatch(a, &log);
}

BOOST_FIXTURE_TEST_CASE(RenameTheBatchDoesNotPair_IsEvictedAndForgotten, disk_fixture)
{
	const res::tag a = at("a.txt");
	const res::tag b = at("b.txt");
	index->put(a, k_a);
	const auto held = rs->require_sync<res::text_resource>(a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned")));
	watch_log log;
	log.watch(*rs, a);

	BOOST_REQUIRE(rs->rename(a, b));
	pump();
	// The owner saw a file go and another come without the same ID.
	index->remove(a);
	index->put(b, k_b);
	apply({});

	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(rs->pinned_count() == 0u);
	BOOST_TEST(log.told == names({ a }));
	rs->unwatch(a, &log);
}

BOOST_FIXTURE_TEST_CASE(TwoBatches_TheFirstDoesNotDecideTheSecond, disk_fixture)
{
	const res::tag a = at("a.txt");
	const res::tag b = at("b.txt");
	const res::tag c = at("c.txt");
	const res::tag d = at("d.txt");
	index->put(a, k_a);
	index->put(c, k_b);
	const auto held_a = rs->require_sync<res::text_resource>(a);
	const auto held_c = rs->require_sync<res::text_resource>(c);
	watch_log log;
	log.watch(*rs, a);
	log.watch(*rs, c);

	BOOST_REQUIRE(rs->rename(a, b));
	pump();
	const std::uint64_t first = rs->delivering_batch();
	BOOST_REQUIRE(rs->rename(c, d));
	pump();
	const std::uint64_t second = rs->delivering_batch();
	BOOST_TEST(first < second);

	// The owner has taken in the first batch only: the second's move is not decided by it.
	index->move(a, b);
	const moves first_moves{ { a, b } };
	rs->apply_asset_batch(first, first_moves);
	pump();
	BOOST_TEST(rs->waiting_for_index_count() == 1u);
	BOOST_TEST(log.told.empty());

	index->move(c, d);
	const moves second_moves{ { c, d } };
	rs->apply_asset_batch(second, second_moves);
	pump();
	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(log.told.empty());
	BOOST_TEST(rs->require_sync<res::text_resource>(b).get_control_block() == held_a.get_control_block());
	BOOST_TEST(rs->require_sync<res::text_resource>(d).get_control_block() == held_c.get_control_block());
	rs->unwatch(a, &log);
	rs->unwatch(c, &log);
}

BOOST_FIXTURE_TEST_CASE(ABatchQueuedBeforeARename_DoesNotDecideIt, disk_fixture)
{
	const res::tag a = at("a.txt");
	const res::tag b = at("b.txt");
	index->put(a, k_a);
	const auto held = rs->require_sync<res::text_resource>(a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned")));
	std::vector<std::uint64_t> numbers;
	int owner = 0;
	rs->subscribe_changes(res::tag::default_protocol(), &owner,
		[&](std::span<const res::file_change>) { numbers.push_back(rs->delivering_batch()); });

	// Another file's batch is made and waits in the main queue when the rename comes.
	BOOST_REQUIRE(rs->store_sync(at("other.txt"), test_util::to_bytes("x")));
	BOOST_REQUIRE(rs->rename(a, b));
	pump();
	BOOST_REQUIRE(numbers.size() == 2u);

	// The owner has taken in the other file's batch only.
	rs->apply_asset_batch(numbers[0], {});
	pump();
	BOOST_TEST(rs->waiting_for_index_count() == 1u);
	BOOST_TEST(rs->pinned_count() == 1u);

	index->move(a, b);
	const moves moved{ { a, b } };
	rs->apply_asset_batch(numbers[1], moved);
	pump();
	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(rs->pinned_count() == 1u);
	BOOST_TEST(rs->require_sync<res::text_resource>(b).get_sync()->str() == "pinned");

	rs->unsubscribe_changes(&owner);
	rs->unpin_resource(b);
	BOOST_TEST(rs->pinned_count() == 0u);
}

BOOST_FIXTURE_TEST_CASE(FailedRemoveOrRenameOfAFileGoneFromDisk_KeepsItsWaitingEntry, quiet_disk_fixture)
{
	const res::tag a{ "res://a.txt" };
	const res::tag c{ "res://c.txt" };
	index->put(a, k_a);
	index->put(c, k_b);
	const auto held_a = rs->require_sync<res::text_resource>(a);
	const auto held_c = rs->require_sync<res::text_resource>(c);
	watch_log log;
	log.watch(*rs, a);
	log.watch(*rs, c);

	// Deleted outside the editor; the watcher's signal registers both before their batch is decided.
	fs::remove(dir.path() / "res" / "a.txt");
	fs::remove(dir.path() / "res" / "c.txt");
	rs->signal_changed(a);
	rs->signal_changed(c);
	BOOST_TEST(rs->waiting_for_index_count() == 2u);

	// The editor removes or renames them meanwhile; the resolver refuses (no file), the entries stay.
	BOOST_TEST(!rs->remove(a));
	BOOST_TEST(!rs->rename(c, res::tag{ "res://d.txt" }));
	BOOST_TEST(rs->waiting_for_index_count() == 2u);

	// Decided all the same: evicted once the index goes.
	rs->set_asset_index(nullptr);
	pump();
	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(std::ranges::count(log.told, std::string{ a.view() }) == 1);
	BOOST_TEST(std::ranges::count(log.told, std::string{ c.view() }) == 1);
	rs->unwatch(a, &log);
	rs->unwatch(c, &log);
}

BOOST_FIXTURE_TEST_CASE(AFailedRemoveAfterOneThatWasDone_DoesNotUndoIt, quiet_disk_fixture)
{
	const res::tag a{ "res://a.txt" };
	index->put(a, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned")));

	BOOST_REQUIRE(rs->remove(a));
	BOOST_TEST(!rs->remove(a));
	BOOST_TEST(rs->waiting_for_index_count() == 1u);

	// The removal that was done still forgets the file.
	rs->set_asset_index(nullptr);
	pump();
	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(rs->pinned_count() == 0u);
}

BOOST_FIXTURE_TEST_CASE(AFailedRenameOfAFileOnDisk_LeavesNothingWaiting, quiet_disk_fixture)
{
	const res::tag a{ "res://a.txt" };
	index->put(a, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned")));

	// Never over an existing file: refused, the file stays where it is.
	BOOST_TEST(!rs->rename(a, res::tag{ "res://c.txt" }));
	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(rs->pinned_count() == 1u);
	rs->unpin_resource(a);
}

BOOST_FIXTURE_TEST_CASE(RemovedAndMadeAgainBeforeTheBatch_KeepsItsPin, disk_fixture)
{
	const res::tag a = at("a.txt");
	index->put(a, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned")));

	BOOST_REQUIRE(rs->remove(a));
	BOOST_REQUIRE(rs->store_sync(a, test_util::to_bytes("again")));
	BOOST_TEST(rs->waiting_for_index_count() == 1u);

	// The index changes before any batch is decided: the file is there again, so it is not forgotten.
	rs->set_asset_index(nullptr);
	pump();
	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(rs->pinned_count() == 1u);
	rs->unpin_resource(a);
	BOOST_TEST(rs->pinned_count() == 0u);
}

BOOST_AUTO_TEST_CASE(RemoveOnAMountThatReportsNothing_IsDecidedByTheNextBatch)
{
	// res:// reports no changes; memory:// does: its batch takes in the waiting removal all the same.
	test_util::temp_dir dir{ "res_asset_index_quiet" };
	dir.write("res/a.txt", "A");
	fs::create_directories(dir.path() / "memory");
	ds::task_system tasks{ test_util::manual_tasks() };
	res::watch_options reporting;
	reporting.report_changes = true;
	res::resource_system sys(tasks, res::mount_table{
		res::mount{ res::tag::default_protocol(), { dir.path() / "res" }, res::mount_access::read_write,
			res::write_policy::allow_all(), res::watch_options{}, true },
		res::mount{ res::tag::memory, { dir.path() / "memory" }, res::mount_access::read_write,
			res::write_policy::allow_all(), std::move(reporting), false },
	});
	auto index = std::make_shared<fake_index>();
	const res::tag a{ "res://a.txt" };
	index->put(a, k_a);
	sys.set_asset_index(index);
	const auto held = sys.require_sync<res::text_resource>(a);
	sys.pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned")));

	BOOST_REQUIRE(sys.remove(a));
	BOOST_TEST(sys.waiting_for_index_count() == 1u);
	BOOST_REQUIRE(sys.store_sync(res::tag{ "memory://other.txt" }, test_util::to_bytes("x")));
	test_util::pump(tasks);

	index->remove(a);
	sys.apply_asset_batch(sys.delivering_batch(), {});
	test_util::pump(tasks);
	BOOST_TEST(sys.waiting_for_index_count() == 0u);
	BOOST_TEST(sys.pinned_count() == 0u);

	sys.set_asset_index(nullptr);
}

BOOST_FIXTURE_TEST_CASE(AliasTargetMoved_ResolvesToItsNewPath, disk_fixture)
{
	const res::tag a = at("a.txt");
	const res::tag b = at("b.txt");
	const res::tag original = at("o.txt");
	index->put(a, k_a);
	rs->register_alias(original, a);
	BOOST_TEST(rs->resolve_alias(original) == a);

	BOOST_REQUIRE(rs->rename(a, b));
	pump();
	index->move(a, b);
	apply({ { a, b } });

	BOOST_TEST(rs->resolve_alias(original) == b);
}

BOOST_AUTO_TEST_CASE(ProjectFileOverAnEngineAssetPath_WinsForAReferenceWithId)
{
	// R28: ID -> path -> the layers as for any request.
	test_util::temp_dir dir{ "res_asset_index_layers" };
	dir.write("engine/a.txt", "engine");
	dir.write("project/res/a.txt", "project");
	ds::task_system tasks{ test_util::manual_tasks() };
	res::resource_system sys(tasks, res::mount_table{
		res::mount{ res::tag::default_protocol(), { dir.path() / "engine", dir.path() / "project" / "res" }, res::mount_access::read_write },
	});
	const res::tag path{ "res://a.txt" };
	auto index = std::make_shared<fake_index>();
	// The engine file's ID (k_a) names the path; the project file over it has an ID of its own (k_b).
	index->put(path, k_a);
	index->put(path, k_b);
	sys.set_asset_index(index);

	for (const res::reference& reference : { res::reference{ k_a, path }, res::reference{ k_a, res::tag{} },
			res::reference{ id_number(99), path } }) {
		const res::tag resolved = sys.resolve_reference(reference);
		BOOST_TEST(resolved == path);
		const auto loaded = sys.require_sync<res::text_resource>(resolved).get_sync();
		BOOST_REQUIRE(loaded);
		BOOST_TEST(loaded->str() == "project");
	}

	sys.set_asset_index(nullptr);
}

// #858: a load in flight under its tag, while the index does not know that tag's ID, and the other spelling live under
// the ID: the rebind keeps both under the ID. The race is real and needs no thread or sleep: the async load waits for
// the task pump while the rebind runs.
BOOST_FIXTURE_TEST_CASE(ALoadInFlightUnderItsTag_StaysUnderTheIdTheOtherSpellingHolds, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag spelled = put("A.txt", "A");
	index->put(a, k_a);
	watch_log log;
	log.watch(*rs, a);
	log.watch(*rs, spelled);

	const auto live = rs->require_sync<res::text_resource>(a);
	const auto in_flight = rs->require<res::text_resource>(spelled);
	BOOST_REQUIRE(!in_flight.is_ready());
	BOOST_REQUIRE(in_flight.get_control_block() != live.get_control_block());

	index->spell(spelled, k_a);
	rs->apply_asset_batch(rs->delivering_batch(), {});

	// Still loading, so still counted: the start of a level waits for it.
	BOOST_TEST(rs->pending_loads() == 1u);
	BOOST_TEST(rs->displaced_count() == 1u);
	// A request gets the live block.
	BOOST_TEST(rs->require<res::text_resource>(spelled).get_control_block() == live.get_control_block());

	pump();
	BOOST_TEST(in_flight.is_ready());
	BOOST_TEST(rs->pending_loads() == 0u);

	// A change of the file reaches both spellings and takes both blocks off the key.
	ram->files[a] = test_util::to_bytes("A2");
	ram->files[spelled] = test_util::to_bytes("A2");
	rs->signal_changed(a);
	pump();
	BOOST_TEST(rs->displaced_count() == 0u);
	std::vector<std::string> told = log.told;
	std::ranges::sort(told);
	std::vector<std::string> expected = names({ a, spelled });
	std::ranges::sort(expected);
	BOOST_TEST(told == expected);
	const auto reloaded = rs->require_sync<res::text_resource>(spelled);
	BOOST_TEST(reloaded.get_control_block() != live.get_control_block());
	BOOST_TEST(reloaded.get_control_block() != in_flight.get_control_block());
	BOOST_TEST(text(spelled) == "A2");
	rs->unwatch(a, &log);
	rs->unwatch(spelled, &log);
}

// #858: over a pinned live block the load still counts and a request gets the pinned one; the pin stays, and the
// displaced block leaves once nothing holds it.
BOOST_FIXTURE_TEST_CASE(ARebindCollision_KeepsTheLiveBlocksPin_AndLetsTheDisplacedOneGo, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag spelled = put("A.txt", "A");
	index->put(a, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned")));
	{
		const auto in_flight = rs->require<res::text_resource>(spelled);
		index->spell(spelled, k_a);
		rs->apply_asset_batch(rs->delivering_batch(), {});
		BOOST_TEST(rs->pending_loads() == 1u);
		BOOST_TEST(rs->pinned_count() == 1u);
		BOOST_TEST(text(spelled) == "pinned");
		pump();
		BOOST_TEST(in_flight.is_ready());
		BOOST_TEST(rs->pending_loads() == 0u);
	}
	pump();
	BOOST_TEST(rs->displaced_count() == 0u);
	BOOST_TEST(rs->pinned_count() == 1u);
	BOOST_TEST(text(a) == "pinned");
	rs->unpin_resource(a);
}

// #858: a change while the displaced block is still loading takes it off the key with the live one.
BOOST_FIXTURE_TEST_CASE(AChangeWhileTheDisplacedBlockLoads_EvictsItWithTheKey, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag spelled = put("A.txt", "A");
	index->put(a, k_a);
	watch_log log;
	log.watch(*rs, spelled);
	const auto live = rs->require_sync<res::text_resource>(a);
	const auto in_flight = rs->require<res::text_resource>(spelled);
	index->spell(spelled, k_a);
	rs->apply_asset_batch(rs->delivering_batch(), {});
	BOOST_REQUIRE(rs->displaced_count() == 1u);

	ram->files[a] = test_util::to_bytes("A2");
	ram->files[spelled] = test_util::to_bytes("A2");
	rs->signal_changed(a);
	BOOST_TEST(rs->displaced_count() == 0u);
	BOOST_TEST(rs->pending_loads() == 0u);
	pump();
	BOOST_TEST(log.told == names({ spelled }));
	const auto reloaded = rs->require_sync<res::text_resource>(spelled);
	BOOST_TEST(reloaded.get_control_block() != live.get_control_block());
	BOOST_TEST(reloaded.get_control_block() != in_flight.get_control_block());
	BOOST_TEST(text(spelled) == "A2");
	rs->unwatch(spelled, &log);
}

// #858: a rebind away from the ID (no index any more) takes the displaced block along: still counted while it loads,
// and a change of either spelling still reaches it.
BOOST_FIXTURE_TEST_CASE(ARebindAwayFromTheId_KeepsTheDisplacedBlock, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag spelled = put("A.txt", "A");
	index->put(a, k_a);
	const auto live = rs->require_sync<res::text_resource>(a);
	const auto in_flight = rs->require<res::text_resource>(spelled);
	index->spell(spelled, k_a);
	rs->apply_asset_batch(rs->delivering_batch(), {});
	BOOST_REQUIRE(rs->displaced_count() == 1u);

	rs->set_asset_index(nullptr);
	BOOST_TEST(rs->displaced_count() == 1u);
	BOOST_TEST(rs->pending_loads() == 1u);
	pump();
	BOOST_TEST(in_flight.is_ready());
	BOOST_TEST(rs->pending_loads() == 0u);

	rs->signal_changed(a);
	rs->signal_changed(spelled);
	BOOST_TEST(rs->displaced_count() == 0u);
	pump();
}

// #869: two spellings of one file, each pinned: one pin of the node, and each spelling takes only its own share away.
BOOST_FIXTURE_TEST_CASE(PinsOfTwoSpellings_AreOnePin_AndEachUnpinTakesItsOwnShare, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag spelled = put("A.txt", "A");
	index->put(a, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned a")));
	rs->pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("pinned spelled")));
	BOOST_TEST(rs->pinned_count() == 2u);

	index->spell(spelled, k_a);
	rs->apply_asset_batch(rs->delivering_batch(), {});
	BOOST_TEST(rs->pinned_count() == 1u);
	BOOST_TEST(text(spelled) == "pinned a");

	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pinned_count() == 1u);
	BOOST_TEST(text(a) == "pinned a");
	rs->unpin_resource(a);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// #869: the pin of a spelling that meets the node's live block holds that block, not the displaced one.
BOOST_FIXTURE_TEST_CASE(APinOfTheOtherSpelling_HoldsTheLiveBlock, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag spelled = put("A.txt", "A");
	index->put(a, k_a);
	auto live = std::make_optional(rs->require_sync<res::text_resource>(a));
	std::weak_ptr live_block = live->get_control_block();
	rs->pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("pinned spelled")));

	index->spell(spelled, k_a);
	rs->apply_asset_batch(rs->delivering_batch(), {});
	BOOST_TEST(rs->pinned_count() == 1u);
	live.reset();
	pump();

	BOOST_REQUIRE(!live_block.expired());
	BOOST_TEST(rs->require_sync<res::text_resource>(spelled).get_control_block() == live_block.lock());
	BOOST_TEST(rs->displaced_count() == 0u);
	BOOST_TEST(text(spelled) == "A");
	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// #869: a warmup is a pin of its spelling too; it goes with that spelling's unpin only.
BOOST_FIXTURE_TEST_CASE(AWarmupOfOneSpelling_GoesWithItsOwnUnpinOnly, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag spelled = put("A.txt", "A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	rs->warmup<res::text_resource>(a);
	rs->warmup<res::text_resource>(spelled);
	pump();
	BOOST_TEST(rs->pinned_count() == 1u);
	rs->unpin_resource(a);
	BOOST_TEST(rs->pinned_count() == 1u);
	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// #869: in a layer (a module's) the spellings own their shares the same way, and the root's pins are apart.
BOOST_FIXTURE_TEST_CASE(PinsOfTwoSpellingsInALayer_AreOwnedApartFromTheRoot, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag spelled = put("A.txt", "A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	res::resource_system layer{ *rs };
	layer.pin_resource(a, res::text_resource(a, test_util::to_bytes("a")));
	layer.pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("spelled")));
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("root")));
	BOOST_TEST(layer.pinned_count() == 1u);

	layer.unpin_resource(spelled);
	pump();
	BOOST_TEST(layer.pinned_count() == 1u);
	rs->unpin_resource(a);
	BOOST_TEST(rs->pinned_count() == 0u);
	BOOST_TEST(layer.pinned_count() == 1u);
	layer.unpin_resource(a);
	BOOST_TEST(layer.pinned_count() == 0u);
}

// #869: owners are per pin, so per type: a spelling takes away only the pins of the types it pinned.
BOOST_FIXTURE_TEST_CASE(OwnersArePerType_AnUnpinTakesOnlyTheTypesItsTagPinned, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag spelled = put("A.txt", "A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("text")));
	rs->pin_resource(spelled, res::binary_resource(spelled, test_util::to_bytes("bytes")));
	BOOST_TEST(rs->pinned_count() == 2u);
	rs->unpin_resource(a);
	BOOST_TEST(rs->pinned_count() == 1u);
	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// #869: one node is one file: the spelling that pinned last put the node's data, and its unpin takes only its share, the
// data stays for the other owner; nothing is loaded.
BOOST_FIXTURE_TEST_CASE(TheSpellingThatPinnedLastUnpins_TheDataStaysForTheOther, index_fixture)
{
	const res::tag a = put("a.txt", "file a");
	const res::tag spelled = put("A.txt", "file A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned a")));
	rs->pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("pinned spelled")));

	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pending_loads() == 0u);
	BOOST_TEST(rs->pinned_count() == 1u);
	BOOST_TEST(text(a) == "pinned spelled");
	rs->unpin_resource(a);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// #869: a rebind back to tags (the index goes) splits the owners: a file change for each, its own file loaded and pinned.
BOOST_FIXTURE_TEST_CASE(ARebindBackToTags_GivesEachSpellingItsOwnFilePinned, index_fixture)
{
	const res::tag a = put("a.txt", "file a");
	const res::tag spelled = put("A.txt", "file A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned a")));
	rs->pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("pinned spelled")));
	BOOST_TEST(rs->pinned_count() == 1u);

	rs->set_asset_index(nullptr);
	pump();
	BOOST_TEST(rs->pinned_count() == 2u);
	BOOST_TEST(text(a) == "file a");
	BOOST_TEST(text(spelled) == "file A");
	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pinned_count() == 1u);
	rs->unpin_resource(a);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// #869: a spelling that gets another ID while a file still holds the old one: both nodes lose the pin and get their own
// file loaded and pinned, counted while it loads; a watcher of either spelling is told.
BOOST_FIXTURE_TEST_CASE(ASpellingThatGetsAnotherId_EachNodeGetsItsOwnFilePinned, index_fixture)
{
	const res::tag a = put("a.txt", "file a");
	const res::tag spelled = put("A.txt", "file A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned a")));
	rs->pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("pinned spelled")));
	watch_log log;
	log.watch(*rs, spelled);

	index->spell(spelled, k_b);
	rs->apply_asset_batch(rs->delivering_batch(), {});
	BOOST_TEST(rs->pinned_count() == 0u);
	BOOST_TEST(rs->pending_loads() == 2u);
	pump();
	BOOST_TEST(rs->pending_loads() == 0u);
	BOOST_TEST(rs->pinned_count() == 2u);
	BOOST_TEST(text(a) == "file a");
	BOOST_TEST(text(spelled) == "file A");
	BOOST_TEST(std::ranges::find(log.told, names({ spelled }).front()) != log.told.end());
	rs->unwatch(spelled, &log);
	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pinned_count() == 1u);
	rs->unpin_resource(a);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// #869: a pin_resource while the split's reload loads is newer: the reload does not overwrite it.
BOOST_FIXTURE_TEST_CASE(APinWhileTheReloadLoads_IsNotOverwritten, index_fixture)
{
	const res::tag a = put("a.txt", "file a");
	const res::tag spelled = put("A.txt", "file A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned a")));
	rs->pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("pinned spelled")));

	index->spell(spelled, k_b);
	rs->apply_asset_batch(rs->delivering_batch(), {});
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned again")));
	pump();
	BOOST_TEST(rs->pinned_count() == 2u);
	BOOST_TEST(text(a) == "pinned again");
	rs->unpin_resource(a);
	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// #869: an unpin while the split's reload loads is final: the reload pins nothing for owners that left.
BOOST_FIXTURE_TEST_CASE(AnUnpinWhileTheReloadLoads_LeavesNoPin, index_fixture)
{
	const res::tag a = put("a.txt", "file a");
	const res::tag spelled = put("A.txt", "file A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned a")));
	rs->pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("pinned spelled")));

	index->spell(spelled, k_b);
	rs->apply_asset_batch(rs->delivering_batch(), {});
	rs->unpin_resource(a);
	pump();
	BOOST_TEST(rs->pinned_count() == 1u);
	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// #869: an owner a split leaves without a file loses its pin: no empty pin stays behind.
BOOST_FIXTURE_TEST_CASE(ASplitOwnerWithoutAFile_LosesThePin, index_fixture)
{
	const res::tag a = put("a.txt", "file a");
	const res::tag spelled = put("A.txt", "file A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned a")));
	rs->pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("pinned spelled")));
	ram->files.erase(a);

	index->spell(spelled, k_b);
	rs->apply_asset_batch(rs->delivering_batch(), {});
	pump();
	BOOST_TEST(rs->pinned_count() == 1u);
	BOOST_TEST(rs->pending_loads() == 0u);
	rs->unpin_resource(a);
	BOOST_TEST(rs->pinned_count() == 1u);
	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// #869: a split in a layer (a module's) reloads the layer's pins the same way, apart from the root's.
BOOST_FIXTURE_TEST_CASE(ASplitInALayer_GivesEachSpellingItsOwnFilePinned, index_fixture)
{
	const res::tag a = put("a.txt", "file a");
	const res::tag spelled = put("A.txt", "file A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	res::resource_system layer{ *rs };
	layer.pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned a")));
	layer.pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("pinned spelled")));
	BOOST_TEST(layer.pinned_count() == 1u);

	index->spell(spelled, k_b);
	rs->apply_asset_batch(rs->delivering_batch(), {});
	pump();
	BOOST_TEST(layer.pinned_count() == 2u);
	BOOST_TEST(rs->pinned_count() == 0u);
	const auto layer_text = [&layer](const res::tag& tag) {
		const auto ptr = layer.require_sync<res::text_resource>(tag).get_sync();
		return ptr ? ptr->str() : std::string{ "<failed>" };
	};
	BOOST_TEST(layer_text(a) == "file a");
	BOOST_TEST(layer_text(spelled) == "file A");
	layer.unpin_resource(a);
	layer.unpin_resource(spelled);
	BOOST_TEST(layer.pinned_count() == 0u);
}

// #869: a second split while the first one's reload loads: the first reload is stale, the second pins each file once.
BOOST_FIXTURE_TEST_CASE(TwoSplitsBeforeTheReloadSettles_PinEachFileOnce, index_fixture)
{
	const res::tag a = put("a.txt", "file a");
	const res::tag spelled = put("A.txt", "file A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned a")));
	rs->pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("pinned spelled")));

	index->spell(spelled, k_b);
	rs->apply_asset_batch(rs->delivering_batch(), {});
	rs->set_asset_index(nullptr);
	pump();
	BOOST_TEST(rs->pending_loads() == 0u);
	BOOST_TEST(rs->pinned_count() == 2u);
	BOOST_TEST(text(a) == "file a");
	BOOST_TEST(text(spelled) == "file A");
	rs->unpin_resource(a);
	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// #869: an owner whose reload still loads moves to another ID without a split: the reload starts again where it is now,
// so the owner is not left without a pin.
BOOST_FIXTURE_TEST_CASE(AnOwnerMovedWhileItsReloadLoads_GetsItsPinWhereItIsNow, index_fixture)
{
	const res::tag a = put("a.txt", "file a");
	const res::tag spelled = put("A.txt", "file A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned a")));
	rs->pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("pinned spelled")));

	index->spell(spelled, k_b);
	rs->apply_asset_batch(rs->delivering_batch(), {});
	index->put(a, id_number(3));
	rs->apply_asset_batch(rs->delivering_batch(), {});
	pump();
	BOOST_TEST(rs->pending_loads() == 0u);
	BOOST_TEST(rs->pinned_count() == 2u);
	BOOST_TEST(text(a) == "file a");
	rs->unpin_resource(a);
	BOOST_TEST(rs->pinned_count() == 1u);
	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// #869: a split owner whose file fails to load loses its pin (with a warning): no empty pin stays behind.
BOOST_FIXTURE_TEST_CASE(ASplitOwnerWhoseFileFailsToLoad_LosesThePin, index_fixture)
{
	rs->registrate_resolver<broken_resolver>("broken");
	const res::tag a{ "broken://t/a.txt" };
	const res::tag spelled{ "broken://t/A.txt" };
	index->put(a, k_a);
	index->spell(spelled, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned a")));
	rs->pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("pinned spelled")));

	index->spell(spelled, k_b);
	rs->apply_asset_batch(rs->delivering_batch(), {});
	pump();
	BOOST_TEST(rs->pending_loads() == 0u);
	BOOST_TEST(rs->pinned_count() == 0u);
	rs->unpin_resource(a);
	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// #869: a split of one file in the root and in a layer: one change signal per node, and the layer's reload shares the
// root's load.
BOOST_FIXTURE_TEST_CASE(ASplitInTheRootAndALayer_SignalsOnceAndLoadsOnce, index_fixture)
{
	const res::tag a = put("a.txt", "file a");
	const res::tag spelled = put("A.txt", "file A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	res::resource_system layer{ *rs };
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("root a")));
	rs->pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("root spelled")));
	layer.pin_resource(a, res::text_resource(a, test_util::to_bytes("layer a")));
	layer.pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("layer spelled")));
	watch_log log;
	log.watch(*rs, a);

	index->spell(spelled, k_b);
	rs->apply_asset_batch(rs->delivering_batch(), {});
	BOOST_TEST(rs->pending_loads() == 2u);
	pump();
	BOOST_TEST(std::ranges::count(log.told, names({ a }).front()) == 1);
	BOOST_TEST(rs->pending_loads() == 0u);
	BOOST_TEST(rs->pinned_count() == 2u);
	BOOST_TEST(layer.pinned_count() == 2u);
	BOOST_TEST(text(a) == "file a");
	rs->unwatch(a, &log);
	layer.unpin_resource(a);
	layer.unpin_resource(spelled);
	rs->unpin_resource(a);
	rs->unpin_resource(spelled);
	BOOST_TEST(rs->pinned_count() == 0u);
	BOOST_TEST(layer.pinned_count() == 0u);
}

// #869: register_alias copies a pin with no owner, so the alias' unpin takes the copy away; where the alias' node has a
// pin of its own owners, the copy does not replace it and the alias' unpin leaves it to them.
BOOST_FIXTURE_TEST_CASE(APinCopiedToAnAlias_GoesWithTheAliasUnpin_UnlessOthersOwnTheAliasPin, index_fixture)
{
	const res::tag original = put("o.txt", "O");
	const res::tag alias = put("al.txt", "AL");
	rs->pin_resource(original, res::text_resource(original, test_util::to_bytes("pinned")));
	rs->register_alias(original, alias);
	BOOST_TEST(rs->pinned_count() == 2u);
	rs->unpin_resource(alias);
	BOOST_TEST(rs->pinned_count() == 1u);

	const res::tag other = put("other.txt", "X");
	const res::tag owned_alias = put("owned.txt", "Y");
	index->put(other, k_b);
	index->spell(owned_alias, k_b);
	rs->pin_resource(other, res::text_resource(other, test_util::to_bytes("other")));
	rs->register_alias(original, owned_alias);
	BOOST_TEST(rs->pinned_count() == 2u);
	rs->unpin_resource(owned_alias);
	BOOST_TEST(rs->pinned_count() == 2u);
	rs->unpin_resource(other);
	rs->unpin_resource(original);
	BOOST_TEST(rs->pinned_count() == 0u);
}

// Regression guard (#869): a file erased is gone under every spelling, so the node's pin goes whoever pinned it.
BOOST_FIXTURE_TEST_CASE(ErasingAFile_TakesThePinOfEverySpellingAway, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag spelled = put("A.txt", "A");
	index->put(a, k_a);
	index->spell(spelled, k_a);
	rs->pin_resource(a, res::text_resource(a, test_util::to_bytes("a")));
	rs->pin_resource(spelled, res::text_resource(spelled, test_util::to_bytes("spelled")));
	BOOST_TEST(rs->pinned_count() == 1u);
	BOOST_TEST(rs->erase(a));
	pump();
	BOOST_TEST(rs->pinned_count() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()


