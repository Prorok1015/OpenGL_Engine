#include <boost/test/unit_test.hpp>
#include <algorithm>
#include "gs_frame_stage.h"

BOOST_AUTO_TEST_SUITE(GsFrameStageTests)

BOOST_AUTO_TEST_CASE(CoreStagesRegisterFrameBeginFirst) {
	gs::frame_stage_registry registry;
	gs::register_core_frame_stages(registry);

	const auto orders = registry.stage_orders();
	BOOST_REQUIRE(!orders.empty());
	BOOST_TEST(orders.front() == gs::frame_begin_order);
	BOOST_TEST(std::count(orders.begin(), orders.end(), gs::frame_begin_order) == 1);
	// Strictly before the poll_events stage and every other core stage.
	BOOST_TEST(gs::frame_begin_order < 10);
	BOOST_TEST(orders.size() >= 2u);
	BOOST_TEST(orders[1] > gs::frame_begin_order);
}

BOOST_AUTO_TEST_CASE(EditorStylesExtraStagesStillOrderedAfterFrameBegin) {
	gs::frame_stage_registry registry;
	gs::register_core_frame_stages(registry);
	registry.add_stage(25, [](gs::frame_step_context&) {});
	registry.add_stage(45, [](gs::frame_step_context&) {});
	BOOST_TEST(registry.stage_orders().front() == gs::frame_begin_order);
}

BOOST_AUTO_TEST_SUITE_END()
