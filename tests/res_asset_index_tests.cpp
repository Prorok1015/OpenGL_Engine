#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include <snk/resource/res_asset_index.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
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

const res::asset_id k_a{ 0x6f1c0b5200004000ull, 0x8000000000000001ull };
const res::asset_id k_b{ 0x6f1c0b5200004000ull, 0x8000000000000002ull };

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

	// A file gone from disk as the watcher reports it: before the index has seen the batch.
	void take_away(const res::tag& tag)
	{
		ram->files.erase(tag);
		rs->signal_changed(tag);
	}

	std::shared_ptr<fake_index> index = std::make_shared<fake_index>();
};

// Counts what a watcher is told, by tag.
struct watch_log
{
	std::vector<std::string> told;
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

BOOST_FIXTURE_TEST_CASE(Rename_KeepsTheLoadedResourceAndItsWatcher, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag b = tag_of("b.txt");
	index->put(a, k_a);

	const auto before = rs->require_sync<res::text_resource>(a);
	watch_log log;
	rs->watch(a, &log, [&log](const res::tag& changed) { log.told.emplace_back(changed.view()); });

	// The watcher's order: the old file is gone, the new one is there, the index learns of it afterwards.
	ram->files[b] = ram->files.at(a);
	take_away(a);
	rs->signal_changed(b);
	BOOST_TEST(rs->waiting_for_index_count() == 1u);
	index->move(a, b);
	const std::pair<res::tag, res::tag> moved[] = { { a, b } };
	rs->apply_asset_batch(moved);
	pump();

	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(log.told.empty());
	const auto after = rs->require_sync<res::text_resource>(b);
	BOOST_TEST(after.get_control_block() == before.get_control_block());

	// A later change of its content still reaches the watcher, under the tag it watched.
	ram->files[b] = test_util::to_bytes("B");
	rs->signal_changed(b);
	pump();
	BOOST_TEST((log.told == std::vector<std::string>{ std::string{ a.view() } }));
	BOOST_TEST(text(b) == "B");

	rs->unwatch(a, &log);
	BOOST_TEST(rs->watched_tag_count() == 0u);
}

BOOST_FIXTURE_TEST_CASE(Rename_KeepsWhatAModuleLayerPinned, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag b = tag_of("b.txt");
	index->put(a, k_a);
	{
		res::resource_system layer(*rs);
		layer.pin_resource(a, res::text_resource(a, test_util::to_bytes("pinned")));
		const auto pinned = layer.require_sync<res::text_resource>(a).get_control_block();

		ram->files[b] = ram->files.at(a);
		take_away(a);
		index->move(a, b);
		const std::pair<res::tag, res::tag> moved[] = { { a, b } };
		rs->apply_asset_batch(moved);
		pump();

		BOOST_TEST(layer.pinned_count() == 1u);
		const auto after = layer.require_sync<res::text_resource>(b);
		BOOST_TEST(after.get_control_block() == pinned);
		BOOST_TEST(after.get_sync()->str() == "pinned");
		layer.unpin_resource(b);
		BOOST_TEST(layer.pinned_count() == 0u);
	}
}

BOOST_FIXTURE_TEST_CASE(DeleteOfAFileWithId_IsEvictedOnceTheBatchCame, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	index->put(a, k_a);
	const auto held = rs->require_sync<res::text_resource>(a);
	watch_log log;
	rs->watch(a, &log, [&log](const res::tag& changed) { log.told.emplace_back(changed.view()); });

	take_away(a);
	pump();
	BOOST_TEST(log.told.empty());
	BOOST_TEST(rs->waiting_for_index_count() == 1u);

	index->remove(a);
	rs->apply_asset_batch({});
	pump();
	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(log.told.size() == 1u);
	rs->unwatch(a, &log);
}

BOOST_FIXTURE_TEST_CASE(WaitingForAMoveThatTheBatchDoesNotName_IsEvicted, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag other = put("other.txt", "O");
	index->put(a, k_a);
	index->put(other, k_b);
	const auto held = rs->require_sync<res::text_resource>(a);
	watch_log log;
	rs->watch(a, &log, [&log](const res::tag& changed) { log.told.emplace_back(changed.view()); });

	take_away(a);
	// A batch about another file decides what waits all the same: it is not kept beyond one batch.
	const std::pair<res::tag, res::tag> moved[] = { { other, tag_of("elsewhere.txt") } };
	rs->apply_asset_batch(moved);
	pump();

	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(log.told.size() == 1u);
	rs->unwatch(a, &log);
}

BOOST_FIXTURE_TEST_CASE(WaitingWithoutABatch_IsEvictedWhenTheIndexIsDetached, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	index->put(a, k_a);
	const auto held = rs->require_sync<res::text_resource>(a);
	watch_log log;
	rs->watch(a, &log, [&log](const res::tag& changed) { log.told.emplace_back(changed.view()); });

	take_away(a);
	BOOST_TEST(rs->waiting_for_index_count() == 1u);
	rs->set_asset_index(nullptr);
	pump();

	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(log.told.size() == 1u);
	// Detached, the watcher keyed by the ID is still found by its tag.
	rs->unwatch(a, &log);
	BOOST_TEST(rs->watched_tag_count() == 0u);
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
	rs->watch(a, &log, [&log](const res::tag& changed) { log.told.emplace_back(changed.view()); });

	ram->files.erase(a);
	rs->signal_changed(a);
	pump();

	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(log.told.size() == 1u);
	BOOST_TEST(rs->resolve_reference(res::reference{ k_a, tag_of("p.txt") }) == tag_of("p.txt"));
	rs->unwatch(a, &log);
}

BOOST_FIXTURE_TEST_CASE(RequestsFromThreads_WhileBatchesApply, index_fixture)
{
	const res::tag a = put("a.txt", "A");
	const res::tag b = tag_of("b.txt");
	ram->files[b] = ram->files.at(a); // both stay readable: only the index moves the ID between them
	index->put(a, k_a);

	constexpr int k_threads = 4;
	constexpr int k_requests = 200;
	std::atomic<bool> go{ false };
	std::vector<std::vector<res::res_handle<res::text_resource>>> handles(k_threads);
	std::vector<std::thread> threads;
	for (int t = 0; t < k_threads; ++t) {
		threads.emplace_back([&, t] {
			while (!go.load()) {
				std::this_thread::yield();
			}
			for (int i = 0; i < k_requests; ++i) {
				handles[t].push_back(rs->require<res::text_resource>(i % 2 == 0 ? a : b));
			}
		});
	}

	go = true;
	res::tag at = a;
	for (int i = 0; i < 50; ++i) {
		const res::tag to = at == a ? b : a;
		index->move(at, to);
		const std::pair<res::tag, res::tag> moved[] = { { at, to } };
		rs->apply_asset_batch(moved);
		at = to;
	}
	for (auto& thread : threads) {
		thread.join();
	}
	pump();

	for (const auto& per_thread : handles) {
		for (const auto& handle : per_thread) {
			BOOST_REQUIRE(handle.is_ready());
			BOOST_TEST(handle.get_sync()->str() == "A");
		}
	}
	BOOST_TEST(rs->waiting_for_index_count() == 0u);
}

BOOST_AUTO_TEST_CASE(ProjectFileOverAnEngineAssetPath_WinsForAReferenceWithId)
{
	// R28: ID -> path -> the layers as for any request.
	test_util::temp_dir dir{ "res_asset_index_layers" };
	dir.write("engine/a.txt", "engine");
	dir.write("project/res/a.txt", "project");
	const fs::path memory = dir.path() / "project" / ".cache" / "memory";
	fs::create_directories(memory);
	ds::task_system tasks{ test_util::manual_tasks() };
	res::resource_system sys(tasks, res::mount_table{
		res::mount{ res::tag::default_protocol(), { dir.path() / "engine", dir.path() / "project" / "res" }, res::mount_access::read_write },
		res::mount{ res::tag::memory, { memory }, res::mount_access::read_write },
	});
	const res::tag path{ "res://a.txt" };
	auto index = std::make_shared<fake_index>();
	// The engine file's ID (k_a) names the path; the project file over it has an ID of its own (k_b).
	index->put(path, k_a);
	index->put(path, k_b);
	sys.set_asset_index(index);

	for (const res::reference& reference : { res::reference{ k_a, path }, res::reference{ k_a, res::tag{} },
			res::reference{ res::asset_id{ 1, 2 }, path } }) {
		const res::tag resolved = sys.resolve_reference(reference);
		BOOST_TEST(resolved == path);
		const auto loaded = sys.require_sync<res::text_resource>(resolved).get_sync();
		BOOST_REQUIRE(loaded);
		BOOST_TEST(loaded->str() == "project");
	}

	sys.set_asset_index(nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
