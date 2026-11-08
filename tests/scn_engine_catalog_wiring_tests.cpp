#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_component_catalog.h>

// scene_init fills the base catalog, and the game layer it creates sees it (#599): the engine registration is not only
// called by the catalog's own tests.

BOOST_FIXTURE_TEST_SUITE(EngineCatalogWiring, test_util::null_engine_fixture)

BOOST_AUTO_TEST_CASE(SceneInitRegistersTheEngineComponents)
{
	auto& contexts = storage.require<scn::context_manager>();
	const scn::component_entry* entry = contexts.catalog().find("scn.local_transform");
	BOOST_REQUIRE(entry != nullptr);
	BOOST_TEST(entry->saved());
	BOOST_REQUIRE(contexts.game_layer() != nullptr);
	BOOST_TEST(contexts.game_layer()->catalog().find("scn.local_transform") == entry);
	BOOST_TEST(contexts.game_layer()->catalog().size() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
