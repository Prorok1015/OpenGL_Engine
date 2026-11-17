#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_prefab_open_watch.h"

// #152 (EPIC-25 US-25-7): the prefab editor waits for a prefab and what it refers to in the background, one poll a frame.

namespace {

using state = edt::prefab_open_watch::state;

struct open_watch_fixture : test_util::scene_env
{
	state pump_until_settled(const edt::prefab_open_watch& watch)
	{
		for (int frame = 0; frame < 100 && watch.poll() == state::loading; ++frame) pump();
		return watch.poll();
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtPrefabOpenWatchTests, open_watch_fixture)

BOOST_AUTO_TEST_CASE(NothingStarted_IsIdle_AndCancelEndsTheWait)
{
	edt::prefab_open_watch watch;
	BOOST_TEST((watch.poll() == state::idle));

	watch.begin(rs->require<scn::prefab_desc>(put("a.prefab.desc", R"({"__type":"prefab_desc","__version":4})")));
	BOOST_TEST(watch.active());
	watch.cancel();

	BOOST_TEST(!watch.active());
	BOOST_TEST((watch.poll() == state::idle));
}

BOOST_AUTO_TEST_CASE(APrefabWithNestedPrefabs_IsLoadingUntilEverythingItReferesToIsThere_ThenReady)
{
	put("leaf.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000f1","name":"tip"}]})");
	put("mid.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000b1","name":"leaf","components":{"ref":"memory://t/leaf.prefab.desc"}}]})");
	const res::tag host = put("host.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000a1","name":"mid","components":{"ref":"memory://t/mid.prefab.desc"}}]})");
	edt::prefab_open_watch watch;

	watch.begin(rs->require<scn::prefab_desc>(host));

	BOOST_TEST((watch.poll() == state::loading));
	BOOST_TEST((pump_until_settled(watch) == state::ready));
	const auto loaded = rs->require<scn::prefab_desc>(host).get();
	BOOST_REQUIRE(loaded);
	BOOST_TEST(!edt::has_pending_dependencies(*loaded));
}

BOOST_AUTO_TEST_CASE(AFileThatIsNotThere_Fails)
{
	edt::prefab_open_watch watch;

	watch.begin(rs->require<scn::prefab_desc>(tag_of("missing.prefab.desc")));

	BOOST_TEST((pump_until_settled(watch) == state::failed));
}

BOOST_AUTO_TEST_SUITE_END()
