#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include <snk/resource/res_asset_index.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// resource_system::hold_changes (#761): what the mounts report waits while a hold is alive and goes the usual way after.

namespace {

class one_id_index : public res::asset_index
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

	void move(const res::tag& from, const res::tag& to)
	{
		std::lock_guard lock(m_mutex);
		const res::asset_id id = m_ids.at(from);
		m_ids.erase(from);
		m_ids[to] = id;
		m_paths[id] = to;
	}

private:
	mutable std::mutex m_mutex;
	std::unordered_map<res::tag, res::asset_id> m_ids;
	std::unordered_map<res::asset_id, res::tag> m_paths;
};

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

// res:// on disk that reports its changes (the batches) and memory:// with no folder, whose store() signals the tag
// through the resolver right away (the path the watcher's finds take).
struct hold_fixture
{
	test_util::temp_dir dir{ "res_change_hold" };
	ds::task_system tasks{ test_util::manual_tasks() };
	std::shared_ptr<one_id_index> index = std::make_shared<one_id_index>();
	std::unique_ptr<res::resource_system> rs;
	std::vector<std::vector<res::file_change>> delivered;
	int owner = 0;

	hold_fixture()
	{
		dir.write("res/a.txt", "A");
		dir.write("res/c.txt", "C");
		res::watch_options watch;
		watch.report_changes = true;
		rs = std::make_unique<res::resource_system>(tasks, res::mount_table{
			res::mount{ res::tag::default_protocol(), { dir.path() / "res" }, res::mount_access::read_write,
				res::write_policy::allow_all(), std::move(watch), true },
			res::mount{ res::tag::memory, {}, res::mount_access::read_write, res::write_policy::allow_all(), res::watch_options{},
				false },
		});
		rs->set_asset_index(index);
		rs->subscribe_changes(res::tag::default_protocol(), &owner, [this](std::span<const res::file_change> batch) {
			delivered.emplace_back(batch.begin(), batch.end());
		});
	}

	~hold_fixture()
	{
		rs->unsubscribe_changes(&owner);
		rs.reset();
	}

	static res::tag at(std::string_view name)
	{
		std::string path{ "res://" };
		path.append(name);
		return res::tag{ std::move(path) };
	}

	static res::tag mem(std::string_view name)
	{
		std::string path{ "memory://t/" };
		path.append(name);
		return res::tag{ std::move(path) };
	}

	void put(const res::tag& tag, std::string_view text) { rs->store(tag, test_util::to_bytes(text)); }

	std::string text(const res::tag& tag)
	{
		const auto ptr = rs->require_sync<res::text_resource>(tag).get_sync();
		return ptr ? ptr->str() : std::string{ "<failed>" };
	}

	void pump() { test_util::pump(tasks); }
};

const res::asset_id k_a{ 0x6f1c0b5200004000ull, 0x8000000000000001ull };

}

BOOST_AUTO_TEST_SUITE(ResChangeHoldTests)

BOOST_FIXTURE_TEST_CASE(ASignalHeldSeveralTimes_IsDeliveredOnceInFirstSeenOrder, hold_fixture)
{
	const res::tag x = mem("x.txt");
	const res::tag y = mem("y.txt");
	put(x, "x1");
	put(y, "y1");
	pump();
	const auto before = rs->require_sync<res::text_resource>(x);
	watch_log log;
	log.watch(*rs, x);
	log.watch(*rs, y);
	{
		const auto hold = rs->hold_changes();
		BOOST_TEST(rs->changes_held());
		put(y, "y2");
		put(x, "x2");
		put(y, "y3");
		pump();
		BOOST_TEST(log.told.empty());
		// Nothing was evicted: the block loaded before is still the one served.
		BOOST_TEST(rs->require_sync<res::text_resource>(x).get_control_block() == before.get_control_block());
	}
	BOOST_TEST(!rs->changes_held());
	pump();
	BOOST_TEST(log.told == names({ y, x }));
	BOOST_TEST(text(x) == "x2");
	BOOST_TEST(text(y) == "y3");
	rs->unwatch(x, &log);
	rs->unwatch(y, &log);
}

BOOST_FIXTURE_TEST_CASE(NestedHolds_OnlyTheLastReleaseDelivers, hold_fixture)
{
	const res::tag x = mem("x.txt");
	put(x, "x1");
	pump();
	watch_log log;
	log.watch(*rs, x);
	auto outer = rs->hold_changes();
	{
		const auto inner = rs->hold_changes();
		put(x, "x2");
	}
	pump();
	BOOST_TEST(rs->changes_held());
	BOOST_TEST(log.told.empty());
	outer.release();
	outer.release();
	pump();
	BOOST_TEST(!rs->changes_held());
	BOOST_TEST(log.told == names({ x }));
	rs->unwatch(x, &log);
}

BOOST_FIXTURE_TEST_CASE(HeldBatches_BecomeOneBatch_OneChangePerPath_InTheOrderFound, hold_fixture)
{
	const res::tag a = at("a.txt");
	const res::tag c = at("c.txt");
	const res::tag n = at("new.txt");
	{
		const auto hold = rs->hold_changes();
		BOOST_REQUIRE(rs->store_sync(a, test_util::to_bytes("A2")));
		BOOST_REQUIRE(rs->store_sync(c, test_util::to_bytes("C2")));
		BOOST_REQUIRE(rs->store_sync(a, test_util::to_bytes("A3")));
		BOOST_REQUIRE(rs->store_sync(n, test_util::to_bytes("N")));
		BOOST_REQUIRE(rs->store_sync(n, test_util::to_bytes("N2")));
		pump();
		BOOST_TEST(delivered.empty());
	}
	pump();
	BOOST_REQUIRE(delivered.size() == 1u);
	const auto& batch = delivered.front();
	BOOST_REQUIRE(batch.size() == 3u);
	BOOST_TEST((batch[0].tag == a));
	BOOST_TEST((batch[0].kind == res::change_kind::modified));
	BOOST_TEST((batch[1].tag == c));
	BOOST_TEST((batch[1].kind == res::change_kind::modified));
	BOOST_TEST((batch[2].tag == n));
	BOOST_TEST((batch[2].kind == res::change_kind::created));
}

BOOST_FIXTURE_TEST_CASE(ARenameDuringAHold_IsDeliveredAsTheSameRename_AndKeepsTheLoadedResource, hold_fixture)
{
	const res::tag a = at("a.txt");
	const res::tag b = at("b.txt");
	index->put(a, k_a);
	const auto before = rs->require_sync<res::text_resource>(a);
	watch_log log;
	log.watch(*rs, a);
	{
		const auto hold = rs->hold_changes();
		BOOST_REQUIRE(rs->rename(a, b));
		pump();
		BOOST_TEST(delivered.empty());
		BOOST_TEST(rs->waiting_for_index_count() == 1u);
	}
	pump();
	BOOST_REQUIRE(delivered.size() == 1u);
	BOOST_REQUIRE(delivered.front().size() == 1u);
	const auto& renamed = delivered.front().front();
	BOOST_TEST((renamed.kind == res::change_kind::renamed));
	BOOST_TEST((renamed.old_tag == a));
	BOOST_TEST((renamed.tag == b));
	BOOST_TEST(renamed.self);

	// The index owner takes the batch in, as after a rename nobody held.
	index->move(a, b);
	const std::vector<std::pair<res::tag, res::tag>> moved{ { a, b } };
	rs->apply_asset_batch(rs->delivering_batch(), moved);
	pump();
	BOOST_TEST(rs->waiting_for_index_count() == 0u);
	BOOST_TEST(log.told.empty());
	BOOST_TEST(rs->require_sync<res::text_resource>(b).get_control_block() == before.get_control_block());
	rs->unwatch(a, &log);
}

BOOST_FIXTURE_TEST_CASE(AHoldInAScopeThatThrows_IsReleased_AndDelivers, hold_fixture)
{
	const res::tag x = mem("x.txt");
	put(x, "x1");
	pump();
	watch_log log;
	log.watch(*rs, x);
	try {
		const auto hold = rs->hold_changes();
		put(x, "x2");
		throw std::runtime_error("unwinds");
	}
	catch (const std::runtime_error&) {
	}
	BOOST_TEST(!rs->changes_held());
	pump();
	BOOST_TEST(log.told == names({ x }));
	rs->unwatch(x, &log);
}

BOOST_FIXTURE_TEST_CASE(ReleasedOnAnotherThread_DeliversOnTheMainThread_AfterWhatCameMeanwhile, hold_fixture)
{
	const res::tag x = mem("x.txt");
	const res::tag y = mem("y.txt");
	put(x, "x1");
	put(y, "y1");
	pump();
	const auto before_y = rs->require_sync<res::text_resource>(y);
	watch_log log;
	log.watch(*rs, x);
	log.watch(*rs, y);
	auto hold = rs->hold_changes();
	put(x, "x2");
	std::thread other([&hold] { hold.release(); });
	other.join();
	// Posted to the main thread: what comes before it runs queues behind the held change.
	BOOST_TEST(!rs->changes_held());
	put(y, "y2");
	BOOST_TEST(rs->require_sync<res::text_resource>(y).get_control_block() == before_y.get_control_block());
	pump();
	BOOST_TEST(log.told == names({ x, y }));
	BOOST_TEST(text(y) == "y2");
	rs->unwatch(x, &log);
	rs->unwatch(y, &log);
}

BOOST_FIXTURE_TEST_CASE(AHoldTakenThroughALayer_HoldsTheRoot, hold_fixture)
{
	const res::tag x = mem("x.txt");
	put(x, "x1");
	pump();
	watch_log log;
	log.watch(*rs, x);
	{
		res::resource_system layer(*rs);
		auto hold = layer.hold_changes();
		BOOST_TEST(rs->changes_held());
		put(x, "x2");
		pump();
		BOOST_TEST(log.told.empty());
		hold.release();
	}
	pump();
	BOOST_TEST(log.told == names({ x }));
	rs->unwatch(x, &log);
}

// signal_changed called by code during a hold tells the watchers now and drops the held signal of that tag: it is told
// once, not again at the release.
BOOST_FIXTURE_TEST_CASE(ASignalFromCodeDuringAHold_TakesTheHeldOneOfItsTag, hold_fixture)
{
	const res::tag x = mem("x.txt");
	const res::tag y = mem("y.txt");
	put(x, "x1");
	put(y, "y1");
	pump();
	watch_log log;
	log.watch(*rs, x);
	log.watch(*rs, y);
	{
		const auto hold = rs->hold_changes();
		put(x, "x2");
		put(y, "y2");
		rs->signal_changed(x);
		pump();
		BOOST_TEST(log.told == names({ x }));
	}
	pump();
	BOOST_TEST(log.told == names({ x, y }));
	rs->unwatch(x, &log);
	rs->unwatch(y, &log);
}

// evict_cached drops what the cache holds and tells no watcher; the change is told when its signal comes.
BOOST_FIXTURE_TEST_CASE(EvictCached_DropsTheEntry_AndTellsNoWatcher, hold_fixture)
{
	const res::tag x = mem("x.txt");
	put(x, "x1");
	pump();
	const auto before = rs->require_sync<res::text_resource>(x);
	watch_log log;
	log.watch(*rs, x);
	{
		const auto hold = rs->hold_changes();
		put(x, "x2");
		rs->evict_cached(x);
		pump();
		BOOST_TEST(log.told.empty());
		BOOST_TEST(rs->require_sync<res::text_resource>(x).get_control_block() != before.get_control_block());
		BOOST_TEST(text(x) == "x2");
	}
	pump();
	BOOST_TEST(log.told == names({ x }));
	rs->unwatch(x, &log);
}

BOOST_AUTO_TEST_SUITE_END()
