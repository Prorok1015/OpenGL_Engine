#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"

namespace vl = rnd::driver::validation;
using test_util::null_engine_fixture;

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendTests, null_engine_fixture)

BOOST_AUTO_TEST_CASE(WindowWithoutGlSurfaceSelectsTheNullBackend)
{
	BOOST_REQUIRE(swapchain());
	BOOST_REQUIRE(driver());
}

BOOST_AUTO_TEST_CASE(RenderValidateWrapsTheBackendInTheValidationDecorator)
{
	BOOST_REQUIRE(validating_swapchain());
	BOOST_REQUIRE(validating_driver());
	BOOST_TEST(&validating_driver()->state() == &validating_swapchain()->state());
}

BOOST_AUTO_TEST_CASE(SmokeLevelRunsThreeFramesWithoutAGpu)
{
	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));
	BOOST_REQUIRE(levels.level_set().size() > 0u);

	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(3) == 3));
	BOOST_TEST(swapchain()->frames_presented() == 3u);
	BOOST_TEST(driver()->calls().draws > 0u);
	BOOST_TEST(driver()->stats().live_total() > 0u);
}

BOOST_AUTO_TEST_CASE(SmokeLevelFramesPassValidationWithoutViolations)
{
	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));

	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(3) == 3));

	const vl::validator& state = validating_driver()->state();
	for (const auto& violation : state.violations()) {
		BOOST_ERROR("frame " << violation.frame << ", " << vl::rule_name(violation.kind) << ": " << violation.message);
	}
	BOOST_TEST(state.total() == 0u);
	BOOST_TEST(state.frame() == 3u);
	BOOST_TEST(state.last_frame().draws > 0u);
}

BOOST_AUTO_TEST_CASE(SmokeLevelBuildsNoFramebufferAfterWarmUp)
{
	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));

	std::vector<std::size_t> built;
	auto& stages = storage.require<app::frame_stage_registry>();
	stages.add_stage("test_framebuffer_probe", app::builtin_stage::present, app::stage_side::after,
		[&](app::stage_context&) { built.push_back(driver()->stats().framebuffers.misses()); });
	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(6) == 6));
	stages.remove_stage("test_framebuffer_probe");

	BOOST_REQUIRE(built.size() == 6u);
	BOOST_TEST(built.back() > 0u);
	for (std::size_t frame = 2; frame < built.size(); ++frame) {
		BOOST_TEST(built[frame] == built[1], "frame " << frame << " built a framebuffer");
	}
	BOOST_TEST(driver()->stats().framebuffers.hits() > 0u);
	BOOST_TEST(driver()->calls().render_pass_begins == driver()->calls().render_pass_ends);
	BOOST_TEST(!driver()->in_render_pass());
}

BOOST_AUTO_TEST_CASE(SmokeLevelBuildsNoPipelineAfterWarmUp)
{
	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));

	std::vector<std::size_t> built;
	auto& stages = storage.require<app::frame_stage_registry>();
	stages.add_stage("test_pipeline_probe", app::builtin_stage::present, app::stage_side::after,
		[&](app::stage_context&) { built.push_back(driver()->pipelines().misses()); });
	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(8) == 8));
	stages.remove_stage("test_pipeline_probe");

	BOOST_REQUIRE(built.size() == 8u);
	BOOST_TEST(built.back() > 0u);
	for (std::size_t frame = 4; frame < built.size(); ++frame) {
		BOOST_TEST(built[frame] == built[3], "frame " << frame << " built a pipeline");
	}
	BOOST_TEST(driver()->pipelines().size() == driver()->pipelines().misses());
	BOOST_TEST(driver()->pipelines().hits() > 0u);
}

BOOST_AUTO_TEST_CASE(SmokeLevelDrawsOnlyThroughBoundPipelines)
{
	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));

	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(3) == 3));

	BOOST_TEST(driver()->calls().pipeline_binds > 0u);
	BOOST_TEST(driver()->calls().draws > 0u);
	BOOST_TEST(validating_driver()->state().count(vl::rule::draw_without_pipeline) == 0u);
	BOOST_TEST(validating_driver()->state().count(vl::rule::pipeline_format_mismatch) == 0u);
	std::size_t by_topology = 0;
	for (auto topology : { rnd::driver::RENDER_MODE::TRIANGLE, rnd::driver::RENDER_MODE::LINE }) {
		by_topology += driver()->draws_with(topology);
	}
	BOOST_TEST(by_topology == driver()->calls().draws);
	BOOST_TEST(driver()->draws_with(rnd::driver::RENDER_MODE::TRIANGLE) > 0u);
}

BOOST_AUTO_TEST_CASE(EveryFrameStartsFromAnInvalidatedStateCache)
{
	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));

	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(3) == 3));

	BOOST_TEST(driver()->calls().state_cache_invalidations == 3u);
}

BOOST_AUTO_TEST_SUITE_END()
