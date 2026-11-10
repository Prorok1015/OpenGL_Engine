#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/scene/scn_model.h>
#include <cstddef>
#include <string>
#include <string_view>

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

struct reference_engine_fixture : test_util::null_engine_fixture
{
	reference_engine_fixture()
		: user(storage.require<res::resource_system>().registrate_resolver<counting_resolver>(res::tag::user))
	{
		user.store(res::tag{ "user://b.png" }, test_util::to_bytes("secret"));
	}

	// Loads a desc the engine's render types know (texture descs are not exported to tests) from memory://.
	res::res_handle<desc::desc_base> load_desc(std::string_view name, std::string_view text)
	{
		auto& rs = storage.require<res::resource_system>();
		const res::tag tag{ std::string{ "memory://t/" } + std::string{ name } };
		rs.store(tag, test_util::to_bytes(text));
		auto handle = rs.require_sync<desc::desc_base>(tag);
		test_util::pump(tasks);
		return handle;
	}

	counting_resolver& user;
};

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendReference, reference_engine_fixture)

// A refused face keeps the cube map loading: its refresh does not hand the empty tag back to get_field_desc.
BOOST_AUTO_TEST_CASE(ACubemapWithARefusedFace_LoadsAndReadsNothing)
{
	const auto single = load_desc("single.desc", R"json({"__type":"texture_cubemap_desc","data":"user://b.png"})json");
	BOOST_TEST(single.is_ready());
	const auto faces = load_desc("faces.desc", R"json({"__type":"texture_cubemap_desc","data":{"right":"user://b.png",
		"left":"memory://t/l.png"}})json");
	BOOST_TEST(faces.is_ready());
	BOOST_TEST(user.resolves == 0u);
}

// A 2D texture that inherits its picture from a parent refreshes it; a refused one stays refused.
BOOST_AUTO_TEST_CASE(ATexture2dInheritingARefusedPicture_LoadsAndReadsNothing)
{
	load_desc("parent.desc", R"json({"__type":"texture_2d_desc","data":"user://b.png"})json");
	const auto child = load_desc("child.desc", R"json({"__type":"texture_2d_desc","__parent":"memory://t/parent.desc"})json");
	BOOST_TEST(child.is_ready());
	BOOST_TEST(user.resolves == 0u);
}

// The smoke level's meshes come from a .glb: without assimp it has no geometry to redirect.
BOOST_AUTO_TEST_CASE(AGeometryTagOfAComponent_IsNotLoadedFromUser_ByTheFrame, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
{
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
