#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "adapters/res_text_adapter.h"
#include "adapters/res_adapter_worker.h"
#include "resources/res_resource_text.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

// Behavior tests for resource_system caching, aliasing, pinning and change notification.
// The "memory" protocol is swapped for a synchronous in-RAM resolver so nothing touches the disk.

namespace {

using test_util::inline_worker;
using test_util::to_bytes;

std::vector<std::byte> bytes_of(std::string_view s) { return to_bytes(s); }

struct sys_fixture : test_util::ram_res_env
{
	sys_fixture() : sys(rs.get()) {}

	res::tag put(std::string_view name, std::string_view content)
	{
		auto tag = tag_of(name);
		BOOST_REQUIRE(rs->store(tag, bytes_of(content)));
		return tag;
	}

	std::string text(const res::tag& tag)
	{
		auto handle = rs->require_sync<res::text_resource>(tag);
		auto ptr = handle.get_sync();
		return ptr ? ptr->str() : std::string("<null>");
	}

	res::resource_system* const sys;
};

}

BOOST_FIXTURE_TEST_SUITE(ResSystemCacheTests, sys_fixture)

BOOST_AUTO_TEST_CASE(StoreThenRequire_RoundTripsContentAndTag)
{
	auto tag = put("round.txt", "hello resource");
	auto handle = sys->require_sync<res::text_resource>(tag);
	BOOST_REQUIRE(handle.is_ready());
	BOOST_TEST(handle.get_tag() == tag);
	BOOST_TEST(handle.get()->str() == "hello resource");
	BOOST_TEST(handle.get()->get_tag() == tag);
}

BOOST_AUTO_TEST_CASE(StoredBytesAreExistsAndFetchableRaw)
{
	auto tag = tag_of("raw.bin");
	BOOST_TEST(!sys->exists(tag));
	put("raw.bin", "abc");
	BOOST_TEST(sys->exists(tag));

	auto raw = sys->fetch_data(tag);
	BOOST_REQUIRE(raw);
	raw->wait();
	BOOST_REQUIRE(raw->is_ready());
	BOOST_TEST(raw->data == bytes_of("abc"));
}

BOOST_AUTO_TEST_CASE(UnknownProtocol_StoreExistsAndFetchFailSoftly)
{
	res::tag bogus{ "nosuchproto://x/y.txt" };
	BOOST_TEST(!sys->store(bogus, bytes_of("x")));
	BOOST_TEST(!sys->exists(bogus));
	BOOST_TEST(!sys->fetch_data(bogus));
}

BOOST_AUTO_TEST_CASE(Require_SameTagWhileHeldSharesOneBlock)
{
	auto tag = put("shared.txt", "v1");
	auto h1 = sys->require_sync<res::text_resource>(tag);
	auto h2 = sys->require<res::text_resource>(tag);
	BOOST_TEST(h1.get_control_block() == h2.get_control_block());
	BOOST_TEST((h1 == h2));
	BOOST_TEST(h1.get() == h2.get()); // literally the same parsed object
}

BOOST_AUTO_TEST_CASE(Require_HeldHandleKeepsServingCachedDataUntilSignalled)
{
	auto tag = put("stale.txt", "v1");
	auto held = sys->require_sync<res::text_resource>(tag);
	put("stale.txt", "v2");
	// Pins current behavior: store() alone does not invalidate the cache.
	BOOST_TEST(text(tag) == "v1");
	BOOST_TEST(held.get()->str() == "v1");
}

BOOST_AUTO_TEST_CASE(Require_AfterAllHandlesDropped_ReloadsFromSource)
{
	auto tag = put("reload.txt", "v1");
	{
		auto h = sys->require_sync<res::text_resource>(tag);
		BOOST_TEST(h.get()->str() == "v1");
	} // weak cache entry expires here
	put("reload.txt", "v2");
	BOOST_TEST(text(tag) == "v2");
}

BOOST_AUTO_TEST_CASE(SignalChanged_EvictsCacheAndKeepsOldHandleValid)
{
	auto tag = put("evict.txt", "v1");
	auto old_handle = sys->require_sync<res::text_resource>(tag);
	put("evict.txt", "v2");

	sys->signal_changed(tag);

	auto fresh = sys->require_sync<res::text_resource>(tag);
	BOOST_TEST(fresh.get_control_block() != old_handle.get_control_block());
	BOOST_TEST(fresh.get()->str() == "v2");
	BOOST_TEST(old_handle.get()->str() == "v1"); // holders of the old handle are not invalidated
}

BOOST_AUTO_TEST_CASE(Watchers_CalledPerOwnerForMatchingTagOnly)
{
	auto tag = tag_of("watched.txt");
	auto other = tag_of("other.txt");
	int a_calls = 0, b_calls = 0;
	res::tag seen;
	int owner_a = 0, owner_b = 0;
	sys->watch(tag, &owner_a, [&](const res::tag& t) { ++a_calls; seen = t; });
	sys->watch(tag, &owner_b, [&](const res::tag&) { ++b_calls; });

	sys->signal_changed(other);
	BOOST_TEST(a_calls == 0);
	BOOST_TEST(b_calls == 0);

	sys->signal_changed(tag);
	BOOST_TEST(a_calls == 1);
	BOOST_TEST(b_calls == 1);
	BOOST_TEST(seen == tag);

	sys->unwatch(tag, &owner_a);
	sys->signal_changed(tag);
	BOOST_TEST(a_calls == 1);
	BOOST_TEST(b_calls == 2);

	// unwatching something never watched is harmless
	BOOST_CHECK_NO_THROW(sys->unwatch(other, &owner_a));
}

BOOST_AUTO_TEST_CASE(Watch_SameOwnerSameTagReplacesCallback)
{
	auto tag = tag_of("replace.txt");
	int first = 0, second = 0, owner = 0;
	sys->watch(tag, &owner, [&](const res::tag&) { ++first; });
	sys->watch(tag, &owner, [&](const res::tag&) { ++second; });
	sys->signal_changed(tag);
	BOOST_TEST(first == 0);
	BOOST_TEST(second == 1);
}

BOOST_AUTO_TEST_CASE(Watcher_MayUnwatchItselfDuringCallback)
{
	auto tag = tag_of("selfunwatch.txt");
	int calls = 0, owner = 0;
	sys->watch(tag, &owner, [&](const res::tag& t) {
		++calls;
		sys->unwatch(t, &owner); // must not deadlock: callbacks run outside the lock
	});
	sys->signal_changed(tag);
	sys->signal_changed(tag);
	BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_CASE(Alias_RedirectsRequireToTargetTag)
{
	auto original = tag_of("old_name.txt");
	auto target = put("new_name.txt", "from target");
	BOOST_TEST(sys->resolve_alias(original) == original);

	sys->register_alias(original, target);
	BOOST_TEST(sys->resolve_alias(original) == target);
	BOOST_TEST(sys->resolve_alias(target) == target);

	auto handle = sys->require_sync<res::text_resource>(original);
	BOOST_TEST(handle.get_tag() == original); // the handle keeps the requested tag (#181)
	BOOST_TEST(handle.get()->str() == "from target");
}

BOOST_AUTO_TEST_CASE(Alias_ReusesAlreadyCachedBlockOfOriginal)
{
	auto original = put("a_orig.txt", "cached");
	auto target = tag_of("a_target.txt"); // never stored on disk
	auto held = sys->require_sync<res::text_resource>(original);

	sys->register_alias(original, target);

	auto via_target = sys->require_sync<res::text_resource>(target);
	BOOST_TEST(via_target.get_control_block() == held.get_control_block());
	BOOST_TEST(via_target.get()->str() == "cached");
}

BOOST_AUTO_TEST_CASE(PinResource_IsServedWithoutLoadAndPersistedWhenMissing)
{
	auto tag = tag_of("pinned.txt");
	BOOST_TEST(!sys->exists(tag));

	sys->pin_resource(tag, res::text_resource{ tag, bytes_of("pinned data") });

	// served straight from the cache: no load involved
	auto handle = sys->require<res::text_resource>(tag);
	BOOST_REQUIRE(handle.is_ready());
	BOOST_TEST(handle.get()->str() == "pinned data");

	// a pinned resource that has no backing file gets serialized through its adapter
	BOOST_TEST(sys->exists(tag));
	auto raw = sys->fetch_data(tag);
	raw->wait();
	BOOST_TEST(raw->data == bytes_of("pinned data"));
}

BOOST_AUTO_TEST_CASE(PinResource_SurvivesHandleReleaseUntilUnpinned)
{
	auto tag = tag_of("pin_lifetime.txt");
	sys->pin_resource(tag, res::text_resource{ tag, bytes_of("v1") });
	put("pin_lifetime.txt", "v2");

	BOOST_TEST(text(tag) == "v1"); // strong cache wins over the file
	sys->unpin_resource(tag);
	BOOST_TEST(text(tag) == "v2"); // nobody holds it any more -> loads the file
}

BOOST_AUTO_TEST_CASE(Warmup_KeepsResourceAliveWithoutExternalHandles)
{
	auto tag = put("warm.txt", "v1");
	sys->warmup<res::text_resource>(tag);
	{
		// warmup's strong-cache push runs in a completion callback; our own callback is queued
		// after it, so once ours fires the resource is pinned.
		auto probe = sys->require_sync<res::text_resource>(tag);
		BOOST_REQUIRE(probe.get_sync());
	}
	put("warm.txt", "v2");
	BOOST_TEST(text(tag) == "v1"); // warmed copy is still cached
	sys->unpin_resource(tag);
	BOOST_TEST(text(tag) == "v2");
}

BOOST_AUTO_TEST_CASE(AsyncWorker_LoadsOnBackgroundThreadAndNotifiesThen)
{
	sys->set_adapter_worker(std::make_unique<core::res::async_adapter_worker>());
	auto tag = put("async.txt", "async payload");

	std::atomic<bool> called{ false };
	auto handle = sys->require<res::text_resource>(tag);
	handle.then([&](res::text_resource& r) {
		if (r.str() == "async payload") called = true;
	});

	auto ptr = handle.get_sync();
	BOOST_REQUIRE(ptr);
	BOOST_TEST(ptr->str() == "async payload");
	// completion callbacks run on the worker thread right after waiters are woken: wait (bounded) for it
	for (int i = 0; i < 500 && !called.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	BOOST_TEST(called.load());

	// replace the worker with the inline one so the fixture can tear down quietly
	sys->set_adapter_worker(std::make_unique<inline_worker>());
}

BOOST_AUTO_TEST_CASE(RequireMissing_YieldsErrorHandleThatIsNotReady)
{
	auto handle = sys->require_sync<res::text_resource>(tag_of("never_stored.txt"));
	BOOST_TEST(!handle.is_ready());
	BOOST_TEST(handle.has_error());
	BOOST_TEST(!handle.get());
	BOOST_TEST(!handle.get_sync());
}

BOOST_AUTO_TEST_SUITE_END()
