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
	auto& levels = storage.require<scn::level_manager>();
	BOOST_REQUIRE(levels.load(res::tag::make("levels/smoke_level.desc")));
	BOOST_REQUIRE(levels.get_level().get_world_count() > 0u);

	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(3) == 3));
	BOOST_TEST(swapchain()->frames_presented() == 3u);
	BOOST_TEST(driver()->calls().draws > 0u);
	BOOST_TEST(driver()->stats().live_total() > 0u);
}

BOOST_AUTO_TEST_CASE(SmokeLevelFramesPassValidationWithoutViolations)
{
	auto& levels = storage.require<scn::level_manager>();
	BOOST_REQUIRE(levels.load(res::tag::make("levels/smoke_level.desc")));

	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(3) == 3));

	const vl::validator& state = validating_driver()->state();
	for (const auto& violation : state.violations()) {
		BOOST_ERROR("frame " << violation.frame << ", " << vl::rule_name(violation.kind) << ": " << violation.message);
	}
	BOOST_TEST(state.total() == 0u);
	BOOST_TEST(state.frame() == 3u);
	BOOST_TEST(state.last_frame().draws > 0u);
}

BOOST_AUTO_TEST_SUITE_END()
