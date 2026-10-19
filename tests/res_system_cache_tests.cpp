#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

// Behavior tests for resource_system caching, aliasing, pinning and change notification.
// The "memory" protocol is swapped for a synchronous in-RAM resolver so nothing touches the disk.

namespace {

// A second type over the same raw bytes as text_resource, so one tag can be requested as two types.
struct shout_resource : res::text_resource
{
	shout_resource(const res::tag& tag, const std::vector<std::byte>& bytes) : res::text_resource(tag, bytes)
	{
		for (auto& c : content) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
};

struct shout_adapter : core::res::adapter_interface
{
	static inline auto INFO = res::adapter_info::make<shout_resource>();

	std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw) const override
	{
		return std::make_shared<shout_resource>(tag, raw);
	}

	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>& resource) const override
	{
		return test_util::to_bytes(std::static_pointer_cast<shout_resource>(resource)->str());
	}
};

using test_util::to_bytes;

std::vector<std::byte> bytes_of(std::string_view s) { return to_bytes(s); }

struct sys_fixture : test_util::ram_res_env
{
	sys_fixture() : sys(rs.get())
	{
		rs->registrate_adapter<shout_adapter>(shout_adapter::INFO);
	}

	res::tag put(std::string_view name, std::string_view content)
	{
		auto tag = tag_of(name);
		BOOST_REQUIRE(rs->store(tag, bytes_of(content)));
		return tag;
	}

	std::string shout(const res::tag& tag)
	{
		auto ptr = rs->require_sync<shout_resource>(tag).get_sync();
		return ptr ? ptr->str() : std::string("<null>");
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
	pump();

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
	pump();
	BOOST_TEST(a_calls == 0);
	BOOST_TEST(b_calls == 0);

	sys->signal_changed(tag);
	pump();
	BOOST_TEST(a_calls == 1);
	BOOST_TEST(b_calls == 1);
	BOOST_TEST(seen == tag);

	sys->unwatch(tag, &owner_a);
	sys->signal_changed(tag);
	pump();
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
	pump();
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
	pump();
	sys->signal_changed(tag);
	pump();
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
	pump(); // the missing-file serialization runs on the compute lane

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
	pump(); // the missing-file serialization runs on the compute lane
	put("pin_lifetime.txt", "v2");

	BOOST_TEST(text(tag) == "v1"); // strong cache wins over the file
	sys->unpin_resource(tag);
	BOOST_TEST(text(tag) == "v2"); // nobody holds it any more -> loads the file
}

BOOST_AUTO_TEST_CASE(Warmup_KeepsResourceAliveWithoutExternalHandles)
{
	auto tag = put("warm.txt", "v1");
	sys->warmup<res::text_resource>(tag);
	pump(); // warmup loads asynchronously and pins from the completion callback
	{
		auto probe = sys->require_sync<res::text_resource>(tag);
		BOOST_REQUIRE(probe.get_sync());
	}
	put("warm.txt", "v2");
	BOOST_TEST(text(tag) == "v1"); // warmed copy is still cached
	sys->unpin_resource(tag);
	BOOST_TEST(text(tag) == "v2");
}

BOOST_AUTO_TEST_CASE(Then_RunsOnTheMainQueueAfterTheLoadTaskFinished)
{
	auto tag = put("then.txt", "then payload");
	ds::lifetime owner;
	std::string seen;
	int calls = 0;

	auto handle = sys->require<res::text_resource>(tag);
	handle.then(owner, [&](res::text_resource& r) { seen = r.str(); ++calls; });

	BOOST_TEST(!handle.is_ready()); // nothing runs until the test drives the lanes
	BOOST_TEST(tasks.run_one(ds::lane::compute));
	BOOST_TEST(handle.is_ready());
	BOOST_TEST(calls == 0); // settled on the lane, delivery waits for the main queue
	BOOST_TEST(tasks.drain_main() == 1u);
	BOOST_TEST(calls == 1);
	BOOST_TEST(seen == "then payload");
}

BOOST_AUTO_TEST_CASE(Then_OnAnAlreadyReadyHandleStillGoesThroughTheMainQueue)
{
	auto tag = put("then_ready.txt", "ready");
	auto handle = sys->require_sync<res::text_resource>(tag);
	ds::lifetime owner;
	int calls = 0;
	handle.then(owner, [&](res::text_resource&) { ++calls; });
	BOOST_TEST(calls == 0);
	pump();
	BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_CASE(Then_IsDroppedWhenTheOwnerIsGone)
{
	auto tag = put("then_gone.txt", "x");
	int calls = 0;
	auto owner = std::make_unique<ds::lifetime>();
	sys->require<res::text_resource>(tag).then(*owner, [&](res::text_resource&) { ++calls; });
	tasks.run_one(ds::lane::compute);
	owner.reset(); // dies between the load and the delivery
	pump();
	BOOST_TEST(calls == 0);
}

BOOST_AUTO_TEST_CASE(Then_IsNotCalledForAFailedLoad)
{
	ds::lifetime owner;
	int calls = 0;
	auto handle = sys->require<res::text_resource>(tag_of("then_missing.txt"));
	handle.then(owner, [&](res::text_resource&) { ++calls; });
	pump();
	BOOST_TEST(handle.has_error());
	BOOST_TEST(calls == 0);
}

BOOST_AUTO_TEST_CASE(Wait_RunsTheQueuedLoadOnAManualSystemAndSettlesEitherWay)
{
	auto good = sys->require<res::text_resource>(put("wait_ok.txt", "ok"));
	auto bad = sys->require<res::text_resource>(tag_of("wait_missing.txt"));
	BOOST_TEST(!good.is_ready());

	good.wait();
	BOOST_TEST(good.is_ready());
	bad.wait(); // no assert, no hang: the failure is for the caller to read
	BOOST_TEST(bad.has_error());
}

BOOST_AUTO_TEST_CASE(SignalChanged_DeliversWatchersOnTheMainQueueNotInline)
{
	auto tag = tag_of("deferred.txt");
	int calls = 0, owner = 0;
	sys->watch(tag, &owner, [&](const res::tag&) { ++calls; });

	sys->signal_changed(tag);
	BOOST_TEST(calls == 0);
	BOOST_TEST(tasks.drain_main() == 1u);
	BOOST_TEST(calls == 1);
}

BOOST_AUTO_TEST_CASE(SignalChanged_EvictsImmediatelyEvenThoughWatchersAreDeferred)
{
	auto tag = put("evict_now.txt", "v1");
	auto old_handle = sys->require_sync<res::text_resource>(tag);
	put("evict_now.txt", "v2");

	sys->signal_changed(tag); // no drain yet
	BOOST_TEST(sys->require_sync<res::text_resource>(tag).get()->str() == "v2");
	pump();
}

BOOST_AUTO_TEST_CASE(SignalChanged_SkipsAWatcherRemovedBeforeTheDrain)
{
	auto tag = tag_of("unwatched_late.txt");
	int calls = 0, owner = 0;
	sys->watch(tag, &owner, [&](const res::tag&) { ++calls; });
	sys->signal_changed(tag);
	sys->unwatch(tag, &owner);
	pump();
	BOOST_TEST(calls == 0);
}

BOOST_AUTO_TEST_CASE(SignalChanged_OnAliasTargetEvictsRequestsMadeThroughTheAlias)
{
	auto original = tag_of("sig_alias_orig.txt");
	auto target = put("sig_alias_target.txt", "v1");
	sys->register_alias(original, target);
	auto via_alias = sys->require_sync<res::text_resource>(original);
	put("sig_alias_target.txt", "v2");

	sys->signal_changed(target);

	BOOST_TEST(sys->require_sync<res::text_resource>(original).get_control_block() != via_alias.get_control_block());
	BOOST_TEST(sys->require_sync<res::text_resource>(original).get()->str() == "v2");
	pump();
}

BOOST_AUTO_TEST_CASE(Alias_KeepsALiveEntryOfTheTarget)
{
	auto original = put("alias_live_orig.txt", "original");
	auto target = put("alias_live_target.txt", "target");
	auto held_original = sys->require_sync<res::text_resource>(original);
	auto held_target = sys->require_sync<res::text_resource>(target);

	sys->register_alias(original, target);

	auto via_target = sys->require_sync<res::text_resource>(target);
	BOOST_TEST(via_target.get_control_block() == held_target.get_control_block());
	BOOST_TEST(via_target.get_control_block() != held_original.get_control_block());
	BOOST_TEST(via_target.get()->str() == "target");
}

BOOST_AUTO_TEST_CASE(Alias_ReplacesAnExpiredEntryOfTheTarget)
{
	auto original = put("alias_dead_orig.txt", "original");
	auto target = put("alias_dead_target.txt", "target");
	auto held_original = sys->require_sync<res::text_resource>(original);
	{
		auto dropped = sys->require_sync<res::text_resource>(target);
		BOOST_REQUIRE(dropped.get_sync());
	} // nobody holds the target's block any more

	sys->register_alias(original, target);

	BOOST_TEST(sys->require_sync<res::text_resource>(target).get_control_block() == held_original.get_control_block());
}

BOOST_AUTO_TEST_CASE(Alias_KeepsAPinnedEntryOfTheTarget)
{
	auto original = tag_of("alias_pin_keep_orig.txt");
	auto target = tag_of("alias_pin_keep_target.txt");
	sys->pin_resource(original, res::text_resource{ original, bytes_of("original pin") });
	sys->pin_resource(target, res::text_resource{ target, bytes_of("target pin") });
	pump();

	sys->register_alias(original, target);

	BOOST_TEST(sys->require_sync<res::text_resource>(target).get()->str() == "target pin");
	sys->unpin_resource(original);
	sys->unpin_resource(target);
}

BOOST_AUTO_TEST_CASE(PerTagOperations_LeaveEveryOtherTagAlone)
{
	std::vector<res::tag> tags;
	std::vector<res::res_handle<res::text_resource>> held;
	for (int i = 0; i < 50; ++i) {
		tags.push_back(put("many_" + std::to_string(i) + ".txt", "v" + std::to_string(i)));
		held.push_back(sys->require_sync<res::text_resource>(tags.back()));
	}

	sys->signal_changed(tags[7]);
	sys->unpin_resource(tags[8]);
	sys->register_alias(tags[9], tag_of("many_alias.txt"));

	for (int i = 0; i < 50; ++i) {
		const bool evicted = i == 7;
		BOOST_TEST((sys->require_sync<res::text_resource>(tags[i]).get_control_block() == held[i].get_control_block()) != evicted);
	}
	pump();
}

BOOST_AUTO_TEST_CASE(CacheKey_SameTagTwoTypesAreCachedSeparately)
{
	auto tag = put("typed.txt", "mixed Case");
	auto as_text = sys->require_sync<res::text_resource>(tag);
	auto as_shout = sys->require_sync<shout_resource>(tag);

	BOOST_TEST(as_text.get_control_block() != as_shout.get_control_block());
	BOOST_TEST(as_text.get()->str() == "mixed Case");
	BOOST_TEST(as_shout.get()->str() == "MIXED CASE");

	auto text_again = sys->require<res::text_resource>(tag);
	auto shout_again = sys->require<shout_resource>(tag);
	BOOST_TEST(text_again.get_control_block() == as_text.get_control_block());
	BOOST_TEST(shout_again.get_control_block() == as_shout.get_control_block());
}

BOOST_AUTO_TEST_CASE(CacheKey_ReverseRequestOrderStillYieldsEachType)
{
	auto tag = put("typed_rev.txt", "abc");
	BOOST_TEST(shout(tag) == "ABC");
	BOOST_TEST(text(tag) == "abc");
}

BOOST_AUTO_TEST_CASE(CacheKey_PinnedResourceIsPerType)
{
	auto tag = tag_of("pinned_typed.txt");
	sys->pin_resource(tag, res::text_resource{ tag, bytes_of("pinned") });
	pump(); // the missing-file serialization runs on the compute lane

	auto pinned = sys->require<res::text_resource>(tag);
	BOOST_REQUIRE(pinned.is_ready());
	BOOST_TEST(pinned.get()->str() == "pinned");

	// the pin covers text_resource only: another type under the same tag loads on its own
	put("pinned_typed.txt", "disk");
	BOOST_TEST(shout(tag) == "DISK");
	BOOST_TEST(text(tag) == "pinned");
}

BOOST_AUTO_TEST_CASE(CacheKey_UnpinReleasesEveryTypeOfTag)
{
	auto tag = put("unpin_typed.txt", "v1");
	sys->warmup<res::text_resource>(tag);
	sys->warmup<shout_resource>(tag);
	pump();
	{
		auto p1 = sys->require_sync<res::text_resource>(tag);
		auto p2 = sys->require_sync<shout_resource>(tag);
		BOOST_REQUIRE(p1.get_sync());
		BOOST_REQUIRE(p2.get_sync());
	}
	put("unpin_typed.txt", "v2");
	BOOST_TEST(text(tag) == "v1");
	BOOST_TEST(shout(tag) == "V1");

	sys->unpin_resource(tag);
	BOOST_TEST(text(tag) == "v2");
	BOOST_TEST(shout(tag) == "V2");
}

BOOST_AUTO_TEST_CASE(SignalChanged_EvictsEveryTypeCachedUnderTag)
{
	auto tag = put("evict_all.txt", "v1");
	auto old_text = sys->require_sync<res::text_resource>(tag);
	auto old_shout = sys->require_sync<shout_resource>(tag);
	put("evict_all.txt", "v2");

	sys->signal_changed(tag);
	pump();

	auto new_text = sys->require_sync<res::text_resource>(tag);
	auto new_shout = sys->require_sync<shout_resource>(tag);
	BOOST_TEST(new_text.get_control_block() != old_text.get_control_block());
	BOOST_TEST(new_shout.get_control_block() != old_shout.get_control_block());
	BOOST_TEST(new_text.get()->str() == "v2");
	BOOST_TEST(new_shout.get()->str() == "V2");
	BOOST_TEST(old_text.get()->str() == "v1");
	BOOST_TEST(old_shout.get()->str() == "V1");
}

BOOST_AUTO_TEST_CASE(SignalChanged_LeavesTypedEntriesOfOtherTagsAlone)
{
	auto changed = put("sig_changed.txt", "a");
	auto untouched = put("sig_untouched.txt", "b");
	auto text_held = sys->require_sync<res::text_resource>(untouched);
	auto shout_held = sys->require_sync<shout_resource>(untouched);

	sys->signal_changed(changed);
	pump();

	BOOST_TEST(sys->require_sync<res::text_resource>(untouched).get_control_block() == text_held.get_control_block());
	BOOST_TEST(sys->require_sync<shout_resource>(untouched).get_control_block() == shout_held.get_control_block());
}

BOOST_AUTO_TEST_CASE(Watchers_FireOncePerSignalRegardlessOfTypesLoaded)
{
	auto tag = put("watch_typed.txt", "v1");
	auto text_held = sys->require_sync<res::text_resource>(tag);
	auto shout_held = sys->require_sync<shout_resource>(tag);
	int calls = 0, owner = 0;
	sys->watch(tag, &owner, [&](const res::tag&) { ++calls; });

	sys->signal_changed(tag);
	pump();
	BOOST_TEST(calls == 1);

	// watching is per tag: a watcher registered before any type was requested is still reached
	auto late = tag_of("watch_late.txt");
	int late_calls = 0, late_owner = 0;
	sys->watch(late, &late_owner, [&](const res::tag&) { ++late_calls; });
	put("watch_late.txt", "x");
	sys->require_sync<shout_resource>(late);
	sys->signal_changed(late);
	pump();
	BOOST_TEST(late_calls == 1);
}

BOOST_AUTO_TEST_CASE(Alias_RedirectsEveryRequestedTypeToTarget)
{
	auto original = tag_of("alias_typed_old.txt");
	auto target = put("alias_typed_new.txt", "Target");
	sys->register_alias(original, target);

	auto as_text = sys->require_sync<res::text_resource>(original);
	auto as_shout = sys->require_sync<shout_resource>(original);
	BOOST_TEST(as_text.get_tag() == original);
	BOOST_TEST(as_shout.get_tag() == original);
	BOOST_TEST(as_text.get()->str() == "Target");
	BOOST_TEST(as_shout.get()->str() == "TARGET");
	BOOST_TEST(as_text.get_control_block() != as_shout.get_control_block());

	// requesting the target directly shares the blocks created through the alias, per type
	BOOST_TEST(sys->require_sync<res::text_resource>(target).get_control_block() == as_text.get_control_block());
	BOOST_TEST(sys->require_sync<shout_resource>(target).get_control_block() == as_shout.get_control_block());
}

BOOST_AUTO_TEST_CASE(Alias_ReusesCachedBlocksOfOriginalForEachType)
{
	auto original = put("alias_cached_orig.txt", "cached");
	auto target = tag_of("alias_cached_target.txt"); // never stored
	auto text_held = sys->require_sync<res::text_resource>(original);
	auto shout_held = sys->require_sync<shout_resource>(original);

	sys->register_alias(original, target);

	BOOST_TEST(sys->require_sync<res::text_resource>(target).get_control_block() == text_held.get_control_block());
	BOOST_TEST(sys->require_sync<shout_resource>(target).get_control_block() == shout_held.get_control_block());
}

BOOST_AUTO_TEST_CASE(Alias_CopiesOnlyTypesThatWereCached)
{
	auto original = put("alias_partial_orig.txt", "data");
	auto target = tag_of("alias_partial_target.txt"); // never stored
	auto text_held = sys->require_sync<res::text_resource>(original);

	sys->register_alias(original, target);

	BOOST_TEST(sys->require_sync<res::text_resource>(target).get_control_block() == text_held.get_control_block());
	auto missing = sys->require_sync<shout_resource>(target);
	BOOST_TEST(missing.has_error()); // shout was never cached for the original and the target has no data
}

BOOST_AUTO_TEST_CASE(Alias_CarriesPinnedEntryOfOriginalToTarget)
{
	auto original = tag_of("alias_pin_orig.txt");
	auto target = tag_of("alias_pin_target.txt");
	sys->pin_resource(original, res::text_resource{ original, bytes_of("pinned") });
	pump(); // the missing-file serialization runs on the compute lane

	sys->register_alias(original, target);
	sys->unpin_resource(original);

	// the alias target keeps its own pin, the original's pin is gone
	auto via_target = sys->require_sync<res::text_resource>(target);
	BOOST_REQUIRE(via_target.get_sync());
	BOOST_TEST(via_target.get()->str() == "pinned");
	sys->unpin_resource(target);
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
