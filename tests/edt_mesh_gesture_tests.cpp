#include <boost/test/unit_test.hpp>
#include "edt_mesh_gesture.h"

using namespace edt::mesh;

BOOST_AUTO_TEST_SUITE(EdtMeshGestureTests)

BOOST_AUTO_TEST_CASE(TheModifiers_GiveTheOperation_CtrlBeatsShift)
{
	BOOST_TEST((select_op_of({}) == select_op::replace));
	BOOST_TEST((select_op_of({ .shift = true }) == select_op::add));
	BOOST_TEST((select_op_of({ .ctrl = true }) == select_op::toggle));
	BOOST_TEST((select_op_of({ .shift = true, .ctrl = true }) == select_op::toggle));
}

BOOST_AUTO_TEST_CASE(APressThatBarelyMoves_IsAClickWhereItWasPressed)
{
	select_gesture gesture(4.f);
	gesture.begin({ 10.f, 20.f }, {});
	gesture.move({ 11.f, 21.f });
	BOOST_TEST(!gesture.dragging_box());

	const gesture_result result = gesture.end({ 11.f, 21.f });
	BOOST_TEST((result.kind == gesture_end::click));
	BOOST_TEST(result.point.x == 10.f);
	BOOST_TEST(result.point.y == 20.f);
	BOOST_TEST((result.op == select_op::replace));
	BOOST_TEST(!gesture.active());
}

BOOST_AUTO_TEST_CASE(AMoveOfTheThreshold_IsABox_WithBothCorners)
{
	select_gesture gesture(4.f);
	gesture.begin({ 50.f, 60.f }, { .shift = true });
	gesture.move({ 54.f, 60.f });
	BOOST_TEST(gesture.dragging_box());

	const gesture_result result = gesture.end({ 90.f, 100.f });
	BOOST_TEST((result.kind == gesture_end::box));
	BOOST_TEST((result.op == select_op::add));
	BOOST_TEST(result.rect.min.x == 50.f);
	BOOST_TEST(result.rect.min.y == 60.f);
	BOOST_TEST(result.rect.max.x == 90.f);
	BOOST_TEST(result.rect.max.y == 100.f);
}

BOOST_AUTO_TEST_CASE(OnceItIsABox_MovingBackDoesNotMakeItAClick)
{
	select_gesture gesture(4.f);
	gesture.begin({ 0.f, 0.f }, {});
	gesture.move({ 30.f, 0.f });
	gesture.move({ 1.f, 0.f });
	BOOST_TEST((gesture.end({ 1.f, 0.f }).kind == gesture_end::box));
}

BOOST_AUTO_TEST_CASE(WhileSelecting_TheCameraDoesNotSeeTheMouse_AfterItDoes)
{
	select_gesture gesture;
	BOOST_TEST(gesture.camera_input());
	gesture.begin({ 0.f, 0.f }, {});
	BOOST_TEST(gesture.selecting());
	BOOST_TEST(!gesture.camera_input());
	gesture.end({ 0.f, 0.f });
	BOOST_TEST(gesture.camera_input());
}

BOOST_AUTO_TEST_CASE(WithAlt_ThePressPansTheCamera_AndSelectsNothing)
{
	select_gesture gesture;
	gesture.begin({ 0.f, 0.f }, { .alt = true });
	BOOST_TEST(gesture.active());
	BOOST_TEST(!gesture.selecting());
	BOOST_TEST(gesture.camera_input());
	gesture.move({ 100.f, 100.f });
	BOOST_TEST(!gesture.dragging_box());
	BOOST_TEST((gesture.end({ 100.f, 100.f }).kind == gesture_end::none));
}

BOOST_AUTO_TEST_CASE(APressWhileOneRuns_IsIgnored_AndCancelEndsTheGesture)
{
	select_gesture gesture;
	gesture.begin({ 1.f, 1.f }, {});
	gesture.begin({ 50.f, 50.f }, { .ctrl = true });
	const gesture_result result = gesture.end({ 1.f, 1.f });
	BOOST_TEST((result.kind == gesture_end::click));
	BOOST_TEST((result.op == select_op::replace));
	BOOST_TEST(result.point.x == 1.f);

	gesture.begin({ 5.f, 5.f }, {});
	gesture.cancel();
	BOOST_TEST(!gesture.active());
	BOOST_TEST((gesture.end({ 5.f, 5.f }).kind == gesture_end::none));
}

BOOST_AUTO_TEST_CASE(ADragToTheUpperLeft_GivesTheBoxWithItsCornersInOrder)
{
	select_gesture gesture(4.f);
	gesture.begin({ 90.f, 100.f }, {});
	gesture.move({ 60.f, 70.f });
	BOOST_REQUIRE(gesture.dragging_box());
	BOOST_TEST(gesture.box().min.x == 60.f);
	BOOST_TEST(gesture.box().max.y == 100.f);

	const gesture_result result = gesture.end({ 50.f, 60.f });
	BOOST_TEST((result.kind == gesture_end::box));
	BOOST_TEST(result.rect.min.x == 50.f);
	BOOST_TEST(result.rect.min.y == 60.f);
	BOOST_TEST(result.rect.max.x == 90.f);
	BOOST_TEST(result.rect.max.y == 100.f);
}

BOOST_AUTO_TEST_CASE(ALostRelease_EndsTheGestureAndGivesTheCameraTheMouseBack)
{
	select_gesture gesture;
	BOOST_TEST(!gesture.release_lost(false));

	gesture.begin({ 5.f, 5.f }, {});
	BOOST_TEST(!gesture.release_lost(true));
	BOOST_TEST(gesture.selecting());
	BOOST_TEST(!gesture.camera_input());

	BOOST_TEST(gesture.release_lost(false));
	BOOST_TEST(!gesture.active());
	BOOST_TEST(gesture.camera_input());
	BOOST_TEST((gesture.end({ 5.f, 5.f }).kind == gesture_end::none));
}

BOOST_AUTO_TEST_SUITE_END()
