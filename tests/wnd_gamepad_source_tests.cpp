#include <boost/test/unit_test.hpp>
#include <snk/window/wnd_gamepad_source.h>

// The gamepad source seam (US-542-B0): what a platform without a device and a test device give.

BOOST_AUTO_TEST_SUITE(WndGamepadSourceTests)

BOOST_AUTO_TEST_CASE(NoDevice_GivesNotConnectedAndAnEmptyState)
{
	wnd::null_gamepad_source source;
	core::inp::gamepad_state state;
	state.connected = true;
	state.buttons[0] = true;
	BOOST_TEST(!source.poll(state));
	BOOST_TEST(!state.connected);
	BOOST_TEST(!state.buttons[0]);
}

BOOST_AUTO_TEST_CASE(ADeviceATestDrives_GivesWhatWasSet_UntilItIsDisconnected)
{
	wnd::fake_gamepad_source source;
	core::inp::gamepad_state state;
	BOOST_TEST(!source.poll(state));

	source.press(core::inp::GAMEPAD_BUTTONS::A);
	source.set_axis(core::inp::GAMEPAD_AXIS_IDS::LEFT_X, -0.75f);
	BOOST_TEST(source.poll(state));
	BOOST_TEST(state.connected);
	BOOST_TEST(state.buttons[static_cast<std::size_t>(core::inp::GAMEPAD_BUTTONS::A)]);
	BOOST_TEST(!state.buttons[static_cast<std::size_t>(core::inp::GAMEPAD_BUTTONS::B)]);
	BOOST_TEST(state.axes[static_cast<std::size_t>(core::inp::GAMEPAD_AXIS_IDS::LEFT_X)] == -0.75f);

	source.press(core::inp::GAMEPAD_BUTTONS::A, false);
	BOOST_TEST(source.poll(state));
	BOOST_TEST(!state.buttons[static_cast<std::size_t>(core::inp::GAMEPAD_BUTTONS::A)]);

	source.disconnect();
	BOOST_TEST(!source.poll(state));
	BOOST_TEST(!state.connected);
}

BOOST_AUTO_TEST_SUITE_END()
