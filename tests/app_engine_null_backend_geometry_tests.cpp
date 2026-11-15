#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/desc/desc_system.h>
#include <snk/scene/scn_geometry_holds.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/scene/scn_model.h>
#include <cstddef>
#include <unordered_map>

// #1021: the geometry a mesh entity draws is read once per load. The root loads it, so the world and the renderer share
// one block; the entity holds it from its assembly on, so the cache still has it when the first frame draws it.

namespace {

class resolves_by_tag : public test_util::ram_resolver
{
public:
	using test_util::ram_resolver::ram_resolver;

	async_raw_data resolve(const res::tag& tag) const override
	{
		++resolves[tag];
		return test_util::ram_resolver::resolve(tag);
	}

	mutable std::unordered_map<res::tag, std::size_t> resolves;
};

const res::tag k_robot_geometry{ "memory://objects/robot/gen_robot.geom.desc" };

}

struct geometry_engine_fixture : test_util::null_engine_fixture
{
	geometry_engine_fixture()
	{
		auto& rs = storage.require<res::resource_system>();
		rs.unregistrate_resolver(res::tag::memory);
		memory = &rs.registrate_resolver<resolves_by_tag>(res::tag::memory);
	}

	std::size_t robot_geometry_reads() const
	{
		const auto found = memory->resolves.find(k_robot_geometry);
		return found == memory->resolves.end() ? 0u : found->second;
	}

	void load_and_draw(std::string_view level, int frames)
	{
		auto& levels = storage.require<scn::context_manager>();
		BOOST_REQUIRE(levels.load_level(res::tag::make(level)));
		test_util::pump(tasks);
		BOOST_REQUIRE(run_frames(frames) == frames);
	}

	// The geometry holds of the level's worlds, summed.
	std::size_t held()
	{
		std::size_t count = 0;
		storage.require<scn::context_manager>().level_set().for_each([&count](scn::world_context& world) {
			if (const auto* holds = world.state().ctx().find<scn::geometry_holds>()) count += holds->size();
		});
		return count;
	}

	resolves_by_tag* memory = nullptr;
};

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendGeometry, geometry_engine_fixture)

// The robot's meshes come from a .glb: without assimp the level has no geometry.
BOOST_AUTO_TEST_CASE(TheGeometryOfALoadedLevel_IsReadOnce, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
{
	load_and_draw("levels/smoke_level.desc", 3);
	BOOST_TEST(held() > 0u);
	BOOST_TEST(robot_geometry_reads() == 1u);

	// Asked for through the root while a world holds it: the same block, not a new read.
	const auto through_root = storage.require<res::resource_system>().require<rnd::geometry_desc>(k_robot_geometry);
	test_util::pump(tasks);
	const res::res_handle<rnd::geometry_desc>* held_handle = nullptr;
	storage.require<scn::context_manager>().level_set().for_each([&held_handle](scn::world_context& world) {
		const auto* holds = world.state().ctx().find<scn::geometry_holds>();
		for (const auto entity : world.state().view<scn::geometry_component>()) {
			if (const auto* handle = holds ? holds->find(entity) : nullptr; handle && !held_handle) held_handle = handle;
		}
	});
	BOOST_REQUIRE(held_handle != nullptr);
	BOOST_TEST((through_root.get_control_block() == held_handle->get_control_block()));
	BOOST_TEST(robot_geometry_reads() == 1u);
}

BOOST_AUTO_TEST_CASE(AfterTheLevelIsGone_ItsGeometryIsLetGo, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
{
	load_and_draw("levels/smoke_level.desc", 3);
	BOOST_REQUIRE(held() > 0u);
	BOOST_REQUIRE(robot_geometry_reads() == 1u);

	// load_level replaces the worlds at once: their registries, and the holds in their ctx, are gone when it returns.
	load_and_draw("levels/test_level.desc", 1);
	BOOST_TEST(held() == 0u);
	// Nothing holds it any more, the renderer neither: asked for again, it is read again.
	const auto again = storage.require<res::resource_system>().require_sync<rnd::geometry_desc>(k_robot_geometry);
	test_util::pump(tasks);
	BOOST_TEST(again.is_ready());
	BOOST_TEST(robot_geometry_reads() == 2u);
}

BOOST_AUTO_TEST_SUITE_END()
