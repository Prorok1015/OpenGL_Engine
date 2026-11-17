#include <boost/test/unit_test.hpp>
#include <snk/gui/gui_platform_backend.h>
#include <snk/window/wnd_null_window_system.h>
#include <snk/window/wnd_native_key.h>
#include <snk/core_input/inp_keyboard_event.hpp>
#include <cfloat>
#include <cstdint>
#include <string>
#include <vector>
#include <imgui.h>

namespace {

using core::inp::KEY_ACTION;
using core::inp::KEYBOARD_BUTTONS;
using core::inp::MOUSE_BUTTONS;

// Headless ImGui context + platform backend driven by a null window (800x600 units, 1600x1200 px).
struct platform_fixture
{
	platform_fixture()
		: window(glm::ivec2{ 800, 600 })
		, backend(window)
	{
		window.set_backbuffer_size(glm::ivec2{ 1600, 1200 });
		ctx = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		// NewFrame asserts on a missing font atlas; no renderer here to build it.
		unsigned char* pixels = nullptr;
		int w = 0, h = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
		backend.init();
	}

	~platform_fixture()
	{
		backend.shutdown();
		ImGui::DestroyContext(ctx);
	}

	template<typename F>
	void frame(F&& body)
	{
		backend.new_frame();
		ImGui::NewFrame();
		body();
		ImGui::EndFrame();
	}

	void frame() { frame([] {}); }

	void send(const inp::input_event& evt) { backend.on_input_event(&window, evt); }

	void key(KEYBOARD_BUTTONS k, KEY_ACTION a)
	{
		inp::input_event e;
		e.construct_payload<core::inp::keyboard_event>(core::inp::keyboard_event{ k, a });
		send(e);
	}

	void cursor(glm::vec2 fb_pos)
	{
		inp::input_event e;
		e.construct_payload<core::inp::cursor_move_event>(core::inp::cursor_move_event{ fb_pos });
		send(e);
	}

	wnd::null_window window;
	gui::platform_backend backend;
	ImGuiContext* ctx = nullptr;
};

}

BOOST_FIXTURE_TEST_SUITE(GuiPlatformBackendTests, platform_fixture)

BOOST_AUTO_TEST_CASE(NewFrameReportsDisplaySizeAndFramebufferScale)
{
	frame();

	const ImGuiIO& io = ImGui::GetIO();
	BOOST_TEST(io.DisplaySize.x == 800.f);
	BOOST_TEST(io.DisplaySize.y == 600.f);
	BOOST_TEST(io.DisplayFramebufferScale.x == 2.f);
	BOOST_TEST(io.DisplayFramebufferScale.y == 2.f);
	BOOST_TEST(io.DeltaTime > 0.f);
	BOOST_TEST((io.BackendFlags & ImGuiBackendFlags_HasMouseCursors) != 0);
	BOOST_TEST(!(io.BackendFlags & ImGuiBackendFlags_HasGamepad));
	BOOST_TEST(!(io.BackendFlags & ImGuiBackendFlags_HasSetMousePos));
}

BOOST_AUTO_TEST_CASE(KeyDownAndUpReachImGui)
{
	key(KEYBOARD_BUTTONS::A, KEY_ACTION::DOWN);
	frame([] { BOOST_TEST(ImGui::IsKeyDown(ImGuiKey_A)); });

	key(KEYBOARD_BUTTONS::A, KEY_ACTION::UP);
	frame([] { BOOST_TEST(!ImGui::IsKeyDown(ImGuiKey_A)); });
}

BOOST_AUTO_TEST_CASE(NavigationAndKeypadKeysAreMapped)
{
	key(KEYBOARD_BUTTONS::LEFT, KEY_ACTION::DOWN);
	key(KEYBOARD_BUTTONS::KP_ENTER, KEY_ACTION::DOWN);
	frame([] {
		BOOST_TEST(ImGui::IsKeyDown(ImGuiKey_LeftArrow));
		BOOST_TEST(!ImGui::IsKeyDown(ImGuiKey_RightArrow));
	});
	frame([] { BOOST_TEST(ImGui::IsKeyDown(ImGuiKey_KeypadEnter)); });
}

BOOST_AUTO_TEST_CASE(PunctuationKeysReachImGui)
{
	struct pair { KEYBOARD_BUTTONS key; ImGuiKey imgui; };
	for (const pair p : { pair{ KEYBOARD_BUTTONS::GRAVE_ACCENT, ImGuiKey_GraveAccent }, pair{ KEYBOARD_BUTTONS::LEFT_BRACKET, ImGuiKey_LeftBracket },
		pair{ KEYBOARD_BUTTONS::RIGHT_BRACKET, ImGuiKey_RightBracket }, pair{ KEYBOARD_BUTTONS::BACKSLASH, ImGuiKey_Backslash } }) {
		key(p.key, KEY_ACTION::DOWN);
		frame([&] { BOOST_TEST(ImGui::IsKeyDown(p.imgui)); });
		key(p.key, KEY_ACTION::UP);
		frame([&] { BOOST_TEST(!ImGui::IsKeyDown(p.imgui)); });
	}
}

BOOST_AUTO_TEST_CASE(EveryGlfwPrintableKeyMapsToEngineKeyAndImGuiKey)
{
	// GLFW key tokens are the ASCII codes of the US-layout key caps.
	std::vector<int> printable = { ' ', '\'', ';', '=', '[', '\\', ']', '`' };
	for (int k = ','; k <= '9'; ++k) printable.push_back(k);
	for (int k = 'A'; k <= 'Z'; ++k) printable.push_back(k);

	for (const int glfw_key : printable) {
		const auto engine_key = wnd::from_native_key(glfw_key);
		BOOST_REQUIRE_MESSAGE(engine_key.has_value(), "GLFW key " << glfw_key);
		BOOST_TEST(wnd::to_native_key(*engine_key) == glfw_key);

		key(*engine_key, KEY_ACTION::DOWN);
		int down = 0;
		frame([&] {
			for (int i = ImGuiKey_NamedKey_BEGIN; i < ImGuiKey_NamedKey_END; ++i) {
				if (ImGui::IsKeyDown(static_cast<ImGuiKey>(i))) ++down;
			}
		});
		BOOST_CHECK_MESSAGE(down == 1, "GLFW key " << glfw_key << " pressed " << down << " ImGui keys");
		key(*engine_key, KEY_ACTION::UP);
		frame();
	}
}

BOOST_AUTO_TEST_CASE(WorldKeysMapToEngineKeysWithoutImGuiCounterpart)
{
	for (const int glfw_key : { 161, 162 }) {
		const auto engine_key = wnd::from_native_key(glfw_key);
		BOOST_REQUIRE(engine_key.has_value());
		BOOST_TEST(wnd::to_native_key(*engine_key) == glfw_key);
	}
}

BOOST_AUTO_TEST_CASE(EveryEngineKeyRoundTripsThroughGlfw)
{
	for (int i = 0; i < static_cast<int>(KEYBOARD_BUTTONS::COUNT); ++i) {
		const auto engine_key = static_cast<KEYBOARD_BUTTONS>(i);
		const int glfw_key = wnd::to_native_key(engine_key);
		BOOST_REQUIRE_MESSAGE(glfw_key != -1, "engine key " << i);
		const auto back = wnd::from_native_key(glfw_key);
		BOOST_REQUIRE_MESSAGE(back.has_value(), "engine key " << i);
		BOOST_CHECK_MESSAGE(*back == engine_key, "engine key " << i);
	}
}

BOOST_AUTO_TEST_CASE(ModifierKeysUpdateModifierState)
{
	key(KEYBOARD_BUTTONS::LEFT_CONTROL, KEY_ACTION::DOWN);
	frame([] {
		BOOST_TEST(ImGui::GetIO().KeyCtrl);
		BOOST_TEST(!ImGui::GetIO().KeyShift);
	});

	key(KEYBOARD_BUTTONS::LEFT_CONTROL, KEY_ACTION::UP);
	frame([] { BOOST_TEST(!ImGui::GetIO().KeyCtrl); });
}

BOOST_AUTO_TEST_CASE(ReleasingOneOfTwoShiftsKeepsShiftHeld)
{
	key(KEYBOARD_BUTTONS::LEFT_SHIFT, KEY_ACTION::DOWN);
	key(KEYBOARD_BUTTONS::RIGHT_SHIFT, KEY_ACTION::DOWN);
	frame();
	frame();

	key(KEYBOARD_BUTTONS::LEFT_SHIFT, KEY_ACTION::UP);
	frame([] { BOOST_TEST(ImGui::GetIO().KeyShift); });
}

BOOST_AUTO_TEST_CASE(CharEventReachesInputQueue)
{
	inp::input_event e;
	e.construct_payload<core::inp::char_event>(core::inp::char_event{ U'q' });
	send(e);

	frame([] {
		const ImGuiIO& io = ImGui::GetIO();
		BOOST_REQUIRE_EQUAL(io.InputQueueCharacters.Size, 1);
		BOOST_TEST(io.InputQueueCharacters[0] == static_cast<ImWchar>('q'));
	});
}

BOOST_AUTO_TEST_CASE(FramebufferCursorBecomesWindowUnits)
{
	cursor(glm::vec2{ 400.f, 200.f });

	frame([] {
		const ImGuiIO& io = ImGui::GetIO();
		BOOST_TEST(io.MousePos.x == 200.f);
		BOOST_TEST(io.MousePos.y == 100.f);
	});
}

BOOST_AUTO_TEST_CASE(CursorLeavingWindowInvalidatesMousePos)
{
	cursor(glm::vec2{ 40.f, 40.f });
	frame([] { BOOST_TEST(ImGui::IsMousePosValid()); });

	inp::input_event e;
	e.construct_payload<core::inp::cursor_entered_event>(core::inp::cursor_entered_event{ false });
	send(e);
	frame([] {
		const ImGuiIO& io = ImGui::GetIO();
		BOOST_TEST(io.MousePos.x == -FLT_MAX);
		BOOST_TEST(!ImGui::IsMousePosValid());
	});
}

BOOST_AUTO_TEST_CASE(MouseButtonAndScrollReachImGui)
{
	inp::input_event click;
	click.construct_payload<core::inp::mouse_click_event>(core::inp::mouse_click_event{ MOUSE_BUTTONS::RIGHT, KEY_ACTION::DOWN });
	send(click);
	frame([] { BOOST_TEST(ImGui::IsMouseDown(ImGuiMouseButton_Right)); });

	inp::input_event scroll;
	scroll.construct_payload<core::inp::scroll_move_event>(core::inp::scroll_move_event{ glm::vec2{ 0.f, 3.f } });
	send(scroll);
	frame([] { BOOST_TEST(ImGui::GetIO().MouseWheel == 3.f); });
}

BOOST_AUTO_TEST_CASE(EventsForAnotherWindowAreIgnored)
{
	inp::input_event e;
	e.construct_payload<core::inp::keyboard_event>(core::inp::keyboard_event{ KEYBOARD_BUTTONS::B, KEY_ACTION::DOWN });
	backend.on_input_event(reinterpret_cast<wnd::handle>(std::uintptr_t{ 1 }), e);

	frame([] { BOOST_TEST(!ImGui::IsKeyDown(ImGuiKey_B)); });
}

BOOST_AUTO_TEST_CASE(FocusLossReleasesHeldKeys)
{
	key(KEYBOARD_BUTTONS::A, KEY_ACTION::DOWN);
	frame([] { BOOST_TEST(ImGui::IsKeyDown(ImGuiKey_A)); });

	backend.on_window_focus_lost(&window);
	frame([] { BOOST_TEST(!ImGui::IsKeyDown(ImGuiKey_A)); });

	backend.on_window_focus_gained(&window);
	frame([] { BOOST_TEST(!ImGui::GetIO().AppFocusLost); });
}

BOOST_AUTO_TEST_CASE(ClipboardRoundTripsThroughWindow)
{
	ImGui::SetClipboardText("from imgui");
	BOOST_TEST(window.get_clipboard_text() == "from imgui");

	window.set_clipboard_text("from os");
	BOOST_TEST(std::string{ ImGui::GetClipboardText() } == "from os");
}

BOOST_AUTO_TEST_CASE(MouseCursorShapeIsForwardedToWindow)
{
	frame([] { ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput); });
	backend.new_frame();
	BOOST_TEST((window.get_cursor_shape() == wnd::cursor_shape::text_input));
	ImGui::NewFrame();
	ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	ImGui::EndFrame();

	backend.new_frame();
	BOOST_TEST((window.get_cursor_shape() == wnd::cursor_shape::resize_ew));
}

BOOST_AUTO_TEST_CASE(NoMouseCursorChangeFlagSuppressesForwarding)
{
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	frame([] { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); });
	backend.new_frame();

	BOOST_TEST((window.get_cursor_shape() == wnd::cursor_shape::arrow));
}

BOOST_AUTO_TEST_CASE(HiddenImGuiCursorHidesWindowCursor)
{
	frame([] { ImGui::SetMouseCursor(ImGuiMouseCursor_None); });
	backend.new_frame();
	BOOST_TEST((window.get_cursor_mode() == wnd::CURSOR_MODE::HIDDEN));

	ImGui::NewFrame();
	ImGui::EndFrame();
	backend.new_frame();
	BOOST_TEST((window.get_cursor_mode() == wnd::CURSOR_MODE::NORMAL));
}

BOOST_AUTO_TEST_SUITE_END()
