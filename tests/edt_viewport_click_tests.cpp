#include <boost/test/unit_test.hpp>
#include "edt_viewport_click.h"
#include <cstdint>
#include <vector>

namespace {

edt::viewport_click_frame frame(bool pressed, bool down, glm::vec2 pixel, bool alt = false, bool shift = false, bool ctrl = false)
{
	return { .takes_input = true, .pressed = pressed, .down = down, .released = !pressed && !down, .pixel = pixel, .modifiers = { shift, ctrl, alt } };
}

}

BOOST_AUTO_TEST_SUITE(EdtViewportClick)

BOOST_AUTO_TEST_CASE(APressAndReleaseInPlace_IsAClickAtThePressPoint)
{
	edt::viewport_click click{ 4.f };
	BOOST_TEST((click.update(frame(true, true, { 10.f, 20.f })).kind == edt::mesh::gesture_end::none));
	BOOST_TEST(!click.camera_input());
	const auto result = click.update(frame(false, false, { 11.f, 20.f }));
	BOOST_TEST((result.kind == edt::mesh::gesture_end::click));
	BOOST_TEST(result.point.x == 10.f);
	BOOST_TEST(click.camera_input());
}

BOOST_AUTO_TEST_CASE(ADragPastTheThreshold_IsABoxNotAClick)
{
	edt::viewport_click click{ 4.f };
	click.update(frame(true, true, { 10.f, 10.f }));
	click.update(frame(false, true, { 40.f, 30.f }));
	BOOST_TEST((click.update(frame(false, false, { 40.f, 30.f })).kind == edt::mesh::gesture_end::box));
}

BOOST_AUTO_TEST_CASE(AltLeft_IsLeftToTheCamera_AndPicksNothing)
{
	edt::viewport_click click{ 4.f };
	click.update(frame(true, true, { 10.f, 10.f }, true));
	BOOST_TEST(click.camera_input());
	BOOST_TEST((click.update(frame(false, false, { 10.f, 10.f }, true)).kind == edt::mesh::gesture_end::none));
}

BOOST_AUTO_TEST_CASE(AltDragPastTheThreshold_MakesNoBox)
{
	edt::viewport_click click{ 4.f };
	click.update(frame(true, true, { 10.f, 10.f }, true));
	click.update(frame(false, true, { 60.f, 40.f }, true));
	BOOST_TEST(click.camera_input());
	BOOST_TEST((click.update(frame(false, false, { 60.f, 40.f }, true)).kind == edt::mesh::gesture_end::none));
}

BOOST_AUTO_TEST_CASE(APressWhereTheViewDoesNotTakeInput_StartsNothing)
{
	edt::viewport_click click{ 4.f };
	auto press = frame(true, true, { 10.f, 10.f });
	press.takes_input = false;
	click.update(press);
	BOOST_TEST(!click.gesture().active());
	BOOST_TEST((click.update(frame(false, false, { 10.f, 10.f })).kind == edt::mesh::gesture_end::none));
}

BOOST_AUTO_TEST_CASE(WhileEditsAreLocked_NothingIsPicked_AndAGestureInFlightEnds)
{
	edt::viewport_click click{ 4.f };
	click.update(frame(true, true, { 10.f, 10.f }));
	auto locked = frame(false, false, { 10.f, 10.f });
	locked.locked = true;
	BOOST_TEST((click.update(locked).kind == edt::mesh::gesture_end::none));
	BOOST_TEST(!click.gesture().active());
	locked.pressed = true;
	locked.down = true;
	click.update(locked);
	BOOST_TEST(!click.gesture().active());
}

BOOST_AUTO_TEST_CASE(ALostRelease_IsNotAClick_AndKeepsTheSelectionAlone)
{
	edt::viewport_click click{ 4.f };
	click.update(frame(true, true, { 10.f, 10.f }));
	auto lost = frame(false, false, { 10.f, 10.f });
	lost.released = false;
	BOOST_TEST((click.update(lost).kind == edt::mesh::gesture_end::none));
	BOOST_TEST(!click.gesture().active());
}

BOOST_AUTO_TEST_CASE(AReleaseAfterTheWindowLostFocus_IsNotAClick)
{
	edt::viewport_click click{ 4.f };
	click.update(frame(true, true, { 10.f, 10.f }));
	auto release = frame(false, false, { 10.f, 10.f });
	release.interrupted = true;
	BOOST_TEST((click.update(release).kind == edt::mesh::gesture_end::none));
	BOOST_TEST(!click.gesture().active());
}

BOOST_AUTO_TEST_CASE(AGestureCancelledByHiding_DoesNotEndAsAClickLater)
{
	edt::viewport_click click{ 4.f };
	click.update(frame(true, true, { 10.f, 10.f }));
	click.cancel();
	BOOST_TEST((click.update(frame(false, false, { 10.f, 10.f })).kind == edt::mesh::gesture_end::none));
	BOOST_TEST(click.camera_input());
}

BOOST_AUTO_TEST_CASE(APressWithTheGizmoHovered_IsNoGestureAndNoClick)
{
	edt::viewport_click click{ 4.f };
	auto press = frame(true, true, { 10.f, 10.f });
	press.takes_input = false;
	click.update(press);
	click.update(frame(false, true, { 12.f, 10.f }));
	BOOST_TEST((click.update(frame(false, false, { 12.f, 10.f })).kind == edt::mesh::gesture_end::none));
}

BOOST_AUTO_TEST_CASE(TheLongestOwnPrefixWins_ForNestedInstances)
{
	const std::vector<std::uint64_t> path{ 1, 2, 3, 4, 5 };
	std::size_t asked = 0;
	std::size_t target_size = 0;
	const bool selected = edt::select_longest_own_prefix(path, [&](const std::vector<std::uint64_t>&, const std::vector<std::uint64_t>& target) {
		++asked;
		target_size = target.size();
		return target.size() >= 2;
	});
	BOOST_TEST(selected);
	BOOST_TEST(asked == 3u);
	BOOST_TEST(target_size == 2u);
}

BOOST_AUTO_TEST_CASE(WhenNoPrefixIsKnown_NothingIsSelected)
{
	const std::vector<std::uint64_t> path{ 1, 2 };
	BOOST_TEST(!edt::select_longest_own_prefix(path, [](const auto&, const auto&) { return false; }));
}

BOOST_AUTO_TEST_CASE(ShiftAdds_CtrlRemoves_CtrlWinsOverShift_NeitherReplaces_ForABoxAndAClick)
{
	const auto op_of_box = [](bool shift, bool ctrl) {
		edt::viewport_click click{ 4.f };
		click.update(frame(true, true, { 10.f, 10.f }, false, shift, ctrl));
		click.update(frame(false, true, { 40.f, 30.f }, false, shift, ctrl));
		const auto result = click.update(frame(false, false, { 40.f, 30.f }, false, shift, ctrl));
		BOOST_REQUIRE((result.kind == edt::mesh::gesture_end::box));
		return edt::pick_op_of(result.op);
	};
	BOOST_TEST((op_of_box(false, false) == edt::pick_op::replace));
	BOOST_TEST((op_of_box(true, false) == edt::pick_op::add));
	BOOST_TEST((op_of_box(false, true) == edt::pick_op::remove));
	BOOST_TEST((op_of_box(true, true) == edt::pick_op::remove));

	edt::viewport_click click{ 4.f };
	click.update(frame(true, true, { 10.f, 10.f }, false, true));
	const auto clicked = click.update(frame(false, false, { 10.f, 10.f }, false, true));
	BOOST_REQUIRE((clicked.kind == edt::mesh::gesture_end::click));
	BOOST_TEST((edt::pick_op_of(clicked.op) == edt::pick_op::add));
}

BOOST_AUTO_TEST_CASE(TheBoxSoFar_IsKnownWhileDragging_ForTheRubberBand)
{
	edt::viewport_click click{ 4.f };
	click.update(frame(true, true, { 30.f, 40.f }));
	BOOST_TEST(!click.gesture().dragging_box());
	click.update(frame(false, true, { 10.f, 10.f }));
	BOOST_REQUIRE(click.gesture().dragging_box());
	BOOST_TEST(click.gesture().box().min.x == 10.f);
	BOOST_TEST(click.gesture().box().max.y == 40.f);
	click.cancel();
	BOOST_TEST(!click.gesture().dragging_box());
}

BOOST_AUTO_TEST_SUITE_END()
