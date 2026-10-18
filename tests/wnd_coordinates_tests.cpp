#include <boost/test/unit_test.hpp>
#include "wnd_coordinates.h"
#include "edt_input_manager.h"

BOOST_AUTO_TEST_SUITE(WndCoordinatesTests)

BOOST_AUTO_TEST_CASE(FramebufferScaleIsOneAtNativeDpi)
{
	BOOST_TEST((wnd::framebuffer_scale({ 1920, 1080 }, { 1920, 1080 }) == glm::vec2{ 1.f }));
}

BOOST_AUTO_TEST_CASE(FramebufferScaleFollowsRatio)
{
	BOOST_TEST((wnd::framebuffer_scale({ 3840, 2160 }, { 1920, 1080 }) == glm::vec2{ 2.f }));
	const auto s = wnd::framebuffer_scale({ 2880, 1620 }, { 1920, 1080 });
	BOOST_TEST(s.x == 1.5f, boost::test_tools::tolerance(1e-6f));
	BOOST_TEST(s.y == 1.5f, boost::test_tools::tolerance(1e-6f));
}

BOOST_AUTO_TEST_CASE(FramebufferScaleOfMinimizedWindowFallsBackToOne)
{
	BOOST_TEST((wnd::framebuffer_scale({ 0, 0 }, { 0, 0 }) == glm::vec2{ 1.f }));
}

BOOST_AUTO_TEST_CASE(CursorPositionIsScaled)
{
	BOOST_TEST((wnd::to_framebuffer({ 100.f, 50.f }, { 2.f, 2.f }) == glm::vec2{ 200.f, 100.f }));
	BOOST_TEST((wnd::to_framebuffer({ 100.f, 50.f }, { 1.f, 1.f }) == glm::vec2{ 100.f, 50.f }));
}

BOOST_AUTO_TEST_CASE(EditorInputAreaIsIdentityAtNativeDpi)
{
	BOOST_TEST((edt::make_input_area({ 10.f, 20.f }, { 300.f, 200.f }, { 1.f, 1.f }) == glm::ivec4{ 10, 20, 310, 220 }));
}

BOOST_AUTO_TEST_CASE(EditorInputAreaScalesToFramebuffer)
{
	BOOST_TEST((edt::make_input_area({ 10.f, 20.f }, { 300.f, 200.f }, { 2.f, 2.f }) == glm::ivec4{ 20, 40, 620, 440 }));
}

BOOST_AUTO_TEST_SUITE_END()
