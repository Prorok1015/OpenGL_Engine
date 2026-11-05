#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include "rnd_drawing_test_context.hpp"
#include "rnd_null_driver.h"
#include "rnd_opaque_pass.h"
#include "rnd_skybox_pass.h"
#include "rnd_transparent_pass.h"
#include "rnd_z_prepass.h"
#include "test_scene_env.hpp"
#include <snk/application/app_frame_stage.h>
#include <snk/render/rnd_frame_context.h>
#include <snk/render/rnd_gpu_blocks.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_time.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <utility>
#include <vector>

// The `time` of the shaders (#544): the time of the camera's world, kept in double and wrapped before it becomes a
// float, so it moves by the frame even after a day of work; every scene pass writes the same value.

namespace {

constexpr double frame = 1.0 / 60.0;

double world_seconds(scn::world_context& world) { return world.state().ctx().get<scn::world_time>().seconds; }

}

BOOST_AUTO_TEST_SUITE(RndShaderTimeTests)

BOOST_AUTO_TEST_CASE(ADayAt60FpsMovesTheShaderTimeByTheFrameWithinAMillisecond)
{
	constexpr std::int64_t frames = 24 * 3600 * 60;
	float previous = rnd::shader_time(0.0);
	double worst_error = 0.0;
	std::int64_t wraps = 0;
	bool in_range = true;
	for (std::int64_t i = 1; i <= frames; ++i) {
		const float time = rnd::shader_time(static_cast<double>(i) * frame);
		in_range = in_range && time >= 0.0f && static_cast<double>(time) < rnd::shader_time_period;
		if (time < previous) {
			++wraps;
		} else {
			worst_error = std::max(worst_error, std::abs(static_cast<double>(time - previous) - frame));
		}
		previous = time;
	}
	BOOST_TEST(in_range);
	BOOST_TEST(worst_error < 0.001);
	BOOST_TEST(wraps == static_cast<std::int64_t>(24 * 3600 / rnd::shader_time_period));
}

BOOST_AUTO_TEST_CASE(UnixSecondsInAFloatStoodStill)
{
	// what the passes wrote before #544: float(Unix seconds) has a step of 128 s in 2026
	const double now = 1.8e9;
	BOOST_TEST(static_cast<float>(now) == static_cast<float>(now + 0.5));
	BOOST_TEST(rnd::shader_time(now) != rnd::shader_time(now + 0.5));
}

BOOST_AUTO_TEST_CASE(SinAndCosOfAnIntegerFrequencyStaySmoothAcrossTheWrap)
{
	const double before = rnd::shader_time_period - frame / 2;
	const double after = rnd::shader_time_period + frame / 2;
	for (const float k : { 1.0f, 2.0f, 7.0f }) {
		BOOST_TEST(std::abs(std::sin(k * rnd::shader_time(after)) - std::sin(k * rnd::shader_time(before))) < 2 * k * frame);
		BOOST_TEST(std::abs(std::cos(k * rnd::shader_time(after)) - std::cos(k * rnd::shader_time(before))) < 2 * k * frame);
	}
}

BOOST_AUTO_TEST_CASE(EveryScenePassWritesTheTimeOfItsCamera)
{
	ds::frame_allocator_reset();
	rnd::driver::null::driver drv;
	test_util::drawing_context context(drv);
	std::pmr::vector<rnd::render_packet_t> packets(ds::frame_allocator());
	packets.reserve(2);
	for (const float time : { 12.5f, 40.25f }) {
		rnd::render_packet_t packet;
		packet.camera.viewport = { 0, 0, 64, 64 };
		packet.camera.color_targets.push_back(res::tag(res::tag::memory, time < 20.0f ? "__shader_time_rt_0" : "__shader_time_rt_1"));
		packet.camera.time = time;
		packet.geometries.push_back(res::tag(res::tag::memory, "shader_time_geometry"));
		rnd::draw_call_t draw;
		draw.geometry.index = 0;
		draw.variant = context.variant();
		draw.material.index = 0;
		draw.indices_count = 3;
		packet.opaque_draws.push_back(draw);
		packet.transparent_draws.push_back(draw);
		packet.skybox_material = rnd::skybox_draw_t{ context.variant(), rnd::material_instance_id{ 0 }, std::nullopt };
		packets.push_back(std::move(packet));
	}
	context.use_packets(&packets);

	rnd::z_prepass z;
	rnd::opaque_pass opaque;
	rnd::skybox_pass sky;
	rnd::transparent_pass transparent;
	for (rnd::render_pass_interface* pass : std::initializer_list<rnd::render_pass_interface*>{ &z, &opaque, &sky, &transparent }) {
		pass->execute(context, drv);
	}

	// one write per camera and pass, in camera order within each pass
	const std::vector<float> expected{ 12.5f, 40.25f, 12.5f, 40.25f, 12.5f, 40.25f, 12.5f, 40.25f };
	BOOST_TEST(context.view_times() == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------- the time of a world

namespace {

struct world_time_fixture : test_util::scene_env
{
	world_time_fixture() : mgr(*rs, *assembler, sf) {}

	scn::world_context& make_world() { mgr.create_set("s"); return mgr.create_context("s", "w", scn::context_kind::runtime); }
	void frames(int count) { for (int i = 0; i < count; ++i) mgr.update(std::chrono::duration<float>(static_cast<float>(frame))); }

	ecs::system_factory sf;
	scn::context_manager mgr;
};

}

BOOST_FIXTURE_TEST_SUITE(ScnWorldTimeTests, world_time_fixture)

BOOST_AUTO_TEST_CASE(ANewWorldStartsAtZeroAndAddsTheDeltaOfEveryFrame)
{
	auto& world = make_world();
	BOOST_TEST(world_seconds(world) == 0.0);
	frames(60);
	BOOST_TEST(world_seconds(world) == 1.0, boost::test_tools::tolerance(1e-5));
}

BOOST_AUTO_TEST_CASE(PauseStopsTheTimeAndStepMovesItByOneFixedStep)
{
	auto& world = make_world();
	frames(30);
	const double at_pause = world_seconds(world);

	mgr.set_mode("s", ecs::run_mode::pause);
	frames(30);
	BOOST_TEST(world_seconds(world) == at_pause);

	mgr.step("s", 3);
	frames(5); // three steps, then the set pauses again
	BOOST_TEST(world_seconds(world) == at_pause + 3 * static_cast<double>(world.stepper().step()), boost::test_tools::tolerance(1e-6));
}

BOOST_AUTO_TEST_CASE(TheTimeScaleScalesTheTime)
{
	auto& world = make_world();
	mgr.set_time_scale("s", 2.0f);
	frames(60);
	BOOST_TEST(world_seconds(world) == 2.0, boost::test_tools::tolerance(1e-5));
}

BOOST_AUTO_TEST_CASE(ResetTimeGoesBackToZero)
{
	auto& world = make_world();
	frames(10);
	BOOST_TEST(world_seconds(world) > 0.0);
	world.reset_time();
	BOOST_TEST(world_seconds(world) == 0.0);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------- the extractor

BOOST_FIXTURE_TEST_SUITE(RndShaderTimeEngineTests, test_util::null_engine_fixture)

BOOST_AUTO_TEST_CASE(EachPacketCarriesTheTimeOfItsWorld)
{
	auto& levels = storage.require<scn::context_manager>();
	auto& first = levels.create_context("level", "shader_time_a", scn::context_kind::level);
	auto& second = levels.create_context("level", "shader_time_b", scn::context_kind::level);
	// frozen, so a frame does not move the times set below
	levels.set_mode("level", ecs::run_mode::pause);
	const auto add_camera = [](scn::world_context& world, const char* target) {
		auto& reg = world.state();
		auto& cam = reg.emplace<scn::camera_component>(reg.create());
		cam.m_viewport = scn::viewport{ glm::ivec4{ 0, 0, 64, 64 } };
		cam.color_targets.push_back(res::tag(res::tag::memory, target));
	};
	add_camera(first, "__shader_time_a_rt");
	add_camera(second, "__shader_time_b_rt");

	std::vector<float> seen;
	auto& stages = storage.require<app::frame_stage_registry>();
	stages.add_stage("shader_time_probe", app::builtin_stage::extract, app::stage_side::after, [&](app::stage_context&) {
		seen.clear();
		for (const auto& packet : storage.require<rnd::frame_context>().data.require<std::pmr::vector<rnd::render_packet_t>>()) {
			seen.push_back(packet.camera.time);
		}
	});

	first.state().ctx().get<scn::world_time>().seconds = 10.0;
	second.state().ctx().get<scn::world_time>().seconds = rnd::shader_time_period + 3.0;
	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(1) == 1));
	BOOST_TEST(seen == (std::vector<float>{ 10.0f, rnd::shader_time(rnd::shader_time_period + 3.0) }), boost::test_tools::per_element());

	// half a second later the shaders see another time
	first.state().ctx().get<scn::world_time>().seconds = 10.5;
	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(1) == 1));
	stages.remove_stage("shader_time_probe");
	BOOST_REQUIRE(!seen.empty());
	BOOST_TEST(seen.front() == 10.5f);
}

BOOST_AUTO_TEST_SUITE_END()
