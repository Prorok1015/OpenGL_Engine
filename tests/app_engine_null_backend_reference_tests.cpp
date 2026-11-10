#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/scene/scn_model.h>
#include <cstddef>

// #697 on the whole engine frame: a geometry tag a component holds goes to the renderer's geometry manager, which loads
// it itself. A component anything wrote (a snapshot, a script, an agent) must not make it read user://.

namespace {

class counting_resolver : public test_util::ram_resolver
{
public:
	using test_util::ram_resolver::ram_resolver;

	async_raw_data resolve(const res::tag& tag) const override
	{
		++resolves;
		return test_util::ram_resolver::resolve(tag);
	}

	mutable std::size_t resolves = 0;
};

}

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendReference, test_util::null_engine_fixture)

// The smoke level's meshes come from a .glb: without assimp it has no geometry to redirect.
BOOST_AUTO_TEST_CASE(AGeometryTagOfAComponent_IsNotLoadedFromUser_ByTheFrame, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
{
	auto& user = storage.require<res::resource_system>().registrate_resolver<counting_resolver>(res::tag::user);
	user.store(res::tag{ "user://saves/slot0.desc" },
		test_util::to_bytes(R"json({"__type":"geometry_desc","layout":{"position":"vec3"},"indices":[0,1,2],"vertices":[]})json"));

	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));
	test_util::pump(tasks);
	BOOST_REQUIRE(run_frames(3) == 3);

	std::size_t redirected = 0;
	levels.level_set().for_each([&redirected](scn::world_context& w) {
		for (auto [e, geometry] : w.state().view<scn::geometry_component>().each()) {
			geometry.geom_tag = res::tag{ "user://saves/slot0.desc" };
			++redirected;
		}
	});
	BOOST_REQUIRE(redirected > 0u);

	BOOST_TEST(run_frames(3) == 3);
	test_util::pump(tasks);
	BOOST_TEST(user.resolves == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
