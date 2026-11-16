#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/common/logger/engine_log.h>
#include <snk/render/rnd_material_groups.h>
#include <cstddef>
#include <string_view>
#include <unordered_map>

// #1030: the textures a level draws with are read once per load, as its geometry is (#1021).

namespace {

class texture_reads_resolver : public test_util::ram_resolver
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

}

struct texture_reads_fixture : test_util::null_engine_fixture
{
	texture_reads_fixture()
	{
		auto& rs = storage.require<res::resource_system>();
		rs.unregistrate_resolver(res::tag::memory);
		memory = &rs.registrate_resolver<texture_reads_resolver>(res::tag::memory);
	}

	texture_reads_resolver* memory = nullptr;
};

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendTextureReads, texture_reads_fixture)

// The robot's textures come from a .glb: without assimp the level has none.
BOOST_AUTO_TEST_CASE(TheTexturesOfALoadedLevel_AreReadOnce, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
{
	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));
	test_util::pump(tasks);
	// Past the frames of placeholder textures, so every texture of the level has been asked for and built.
	constexpr int warm_frames = static_cast<int>(rnd::material_groups::grace_frames) + 2;
	BOOST_REQUIRE(run_frames(warm_frames) == warm_frames);

	std::size_t texture_descs = 0;
	for (const auto& [tag, reads] : memory->resolves) {
		if (reads > 1) egLOG_INFO("test/reads", "#1030 {} read {} times", tag.view(), reads);
		if (!tag.view().ends_with(".txm.desc")) continue;
		++texture_descs;
		BOOST_TEST_CONTEXT(tag.view()) { BOOST_TEST(reads == 1u); }
	}
	BOOST_TEST(texture_descs > 0u);
}

BOOST_AUTO_TEST_SUITE_END()
