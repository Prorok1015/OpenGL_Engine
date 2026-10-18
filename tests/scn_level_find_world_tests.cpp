#include <boost/test/unit_test.hpp>
#include <snk/scene/level/scn_level.h>

// #175: a name kept outside the level (editor world_names) can outlive the world after a level
// reload rebuilds the worlds from the desc. get_world(name) asserts on it; find_world must not.
BOOST_AUTO_TEST_SUITE(ScnLevelFindWorldTests)

BOOST_AUTO_TEST_CASE(FindWorld_ReturnsExistingWorldAndNullForMissing)
{
	scn::level lvl(nullptr);
	auto& w0 = lvl.create_world("world1", 0);
	auto& w1 = lvl.create_world("world2", 1);

	BOOST_CHECK(lvl.find_world("world1") == &w0);
	BOOST_CHECK(lvl.find_world("world2") == &w1);
	BOOST_CHECK(lvl.find_world("nope") == nullptr);
}

BOOST_AUTO_TEST_CASE(FindWorld_NullAfterReloadDropsRuntimeCreatedWorld)
{
	scn::level lvl(nullptr);
	lvl.create_world("world1", 0);
	lvl.create_world("world2", 1);

	// a full reload clears the level and recreates only the desc's worlds
	lvl.clear();
	lvl.create_world("world1", 0);

	BOOST_CHECK(lvl.find_world("world1") != nullptr);
	BOOST_CHECK(lvl.find_world("world2") == nullptr);
	BOOST_CHECK(!lvl.has_world("world2"));
}

BOOST_AUTO_TEST_SUITE_END()
