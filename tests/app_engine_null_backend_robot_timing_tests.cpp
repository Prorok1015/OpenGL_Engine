#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_base.hpp>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/render/rnd_material_groups.h>
#include <snk/render/texture/rnd_texture_desc.h>
#include <chrono>
#include <cstddef>
#include <vector>

// #977 step A: a temporary measurement of where one load of smoke_level spends its time on the null backend, run by
// CI on Windows Debug; it checks nothing and goes before the PR leaves draft.
namespace timing_977 {

struct stopwatch
{
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	double ms() const { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
};

struct robot_timing_fixture : test_util::null_engine_fixture
{
	robot_timing_fixture()
	{
		root().unregistrate_resolver(res::tag::memory);
		memory = &root().registrate_resolver<test_util::ram_resolver>(res::tag::memory);
	}

	res::resource_system& root() { return storage.require<res::resource_system>(); }

	test_util::ram_resolver* memory = nullptr;
};

constexpr int warm_frames = static_cast<int>(rnd::material_groups::grace_frames) + 2;

}

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendRobotTiming, timing_977::robot_timing_fixture)

// The parts one at a time, each kept alive so the next part does not pay for it again.
BOOST_AUTO_TEST_CASE(TheRobotLevel_ByParts, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
{
	using namespace timing_977;
	stopwatch import_clock;
	const auto model = root().require_sync<desc::desc_base>(res::tag{ "res://objects/robot/gen_robot.glb" });
	test_util::pump(tasks);
	const double import_ms = import_clock.ms();
	BOOST_REQUIRE(model.is_ready());

	stopwatch geometry_clock;
	const auto geometry = root().require_sync<rnd::geometry_desc>(res::tag{ "memory://objects/robot/gen_robot.geom.desc" });
	test_util::pump(tasks);
	const double geometry_ms = geometry_clock.ms();
	BOOST_REQUIRE(geometry.is_ready());

	std::vector<res::tag> texture_tags;
	for (const auto& [tag, bytes] : memory->files) {
		if (tag.view().ends_with(".txm.desc")) texture_tags.push_back(tag);
	}
	stopwatch textures_clock;
	std::vector<res::res_handle<rnd::texture_desc>> textures;
	for (const res::tag& tag : texture_tags) textures.push_back(root().require_sync<rnd::texture_desc>(tag));
	test_util::pump(tasks);
	const double textures_ms = textures_clock.ms();

	stopwatch level_clock;
	BOOST_REQUIRE(storage.require<scn::context_manager>().load_level(res::tag::make("levels/smoke_level.desc")));
	test_util::pump(tasks);
	const double level_ms = level_clock.ms();

	stopwatch first_clock;
	BOOST_REQUIRE(run_frames(1) == 1);
	const double first_ms = first_clock.ms();
	stopwatch rest_clock;
	BOOST_REQUIRE(run_frames(warm_frames - 1) == warm_frames - 1);
	const double rest_ms = rest_clock.ms();

	egLOG_INFO("test/timing", "#977 parts: import {:.0f} ms, geometry {:.0f} ms, {} texture descs {:.0f} ms, level with all "
		"that held {:.0f} ms, first frame {:.0f} ms, next {} frames {:.0f} ms", import_ms, geometry_ms, texture_tags.size(),
		textures_ms, level_ms, first_ms, warm_frames - 1, rest_ms);
}

// What a case pays: the level from nothing, as the 30 robot cases load it.
BOOST_AUTO_TEST_CASE(TheRobotLevel_AsACaseLoadsIt, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
{
	using namespace timing_977;
	stopwatch level_clock;
	BOOST_REQUIRE(storage.require<scn::context_manager>().load_level(res::tag::make("levels/smoke_level.desc")));
	const double load_call_ms = level_clock.ms();
	test_util::pump(tasks);
	const double level_ms = level_clock.ms();

	stopwatch first_clock;
	BOOST_REQUIRE(run_frames(1) == 1);
	const double first_ms = first_clock.ms();
	stopwatch rest_clock;
	BOOST_REQUIRE(run_frames(warm_frames - 1) == warm_frames - 1);
	const double rest_ms = rest_clock.ms();

	egLOG_INFO("test/timing", "#977 whole: load_level call {:.0f} ms, with pump {:.0f} ms, first frame {:.0f} ms, next {} "
		"frames {:.0f} ms", load_call_ms, level_ms, first_ms, warm_frames - 1, rest_ms);
}

BOOST_AUTO_TEST_SUITE_END()
