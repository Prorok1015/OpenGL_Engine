#include <boost/test/unit_test.hpp>
#include <snk/input/inp_input_system.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/input/inp_events.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using core::inp::KEY_ACTION;
using core::inp::KEYBOARD_BUTTONS;
using core::inp::MOUSE_BUTTONS;

inp::input_event key_event(KEYBOARD_BUTTONS k, KEY_ACTION a)
{
	inp::input_event e;
	e.construct_payload<core::inp::keyboard_event>(core::inp::keyboard_event{ k, a });
	return e;
}

inp::input_event click_event(MOUSE_BUTTONS b, KEY_ACTION a)
{
	inp::input_event e;
	e.construct_payload<core::inp::mouse_click_event>(core::inp::mouse_click_event{ b, a });
	return e;
}

inp::input_event move_event(glm::vec2 pos)
{
	inp::input_event e;
	e.construct_payload<core::inp::cursor_move_event>(core::inp::cursor_move_event{ pos });
	return e;
}

inp::input_event scroll_event(glm::vec2 dir)
{
	inp::input_event e;
	e.construct_payload<core::inp::scroll_move_event>(core::inp::scroll_move_event{ dir });
	return e;
}

constexpr auto idx(KEYBOARD_BUTTONS k) { return static_cast<std::size_t>(k); }
constexpr auto idx(MOUSE_BUTTONS b) { return static_cast<std::size_t>(b); }

wnd::handle win(std::uintptr_t v) { return reinterpret_cast<wnd::handle>(v); }

struct recording_layer : inp::input_manager_base
{
	recording_layer(std::string n, bool consume, std::vector<std::string>& log)
		: inp::input_manager_base(n), name(std::move(n)), consume(consume), log(log) {}
	bool on_handle_event(wnd::handle, const inp::input_event&) override
	{
		log.push_back(name);
		return consume;
	}
	std::string name;
	bool consume;
	std::vector<std::string>& log;
};

struct recording_listener : inp::input_event_consumable_listener_interface
{
	explicit recording_listener(bool consume) : consume(consume) {}
	bool on_handle_event(wnd::handle, const inp::input_event&) override { ++calls; return consume; }
	bool consume;
	int calls = 0;
};

}

BOOST_AUTO_TEST_SUITE(InpInputTests)

// ---------------------------------------------------------------- input_event

BOOST_AUTO_TEST_CASE(Event_PayloadIsTypeChecked)
{
	auto e = key_event(KEYBOARD_BUTTONS::W, KEY_ACTION::DOWN);
	auto* k = e.get_payload<core::inp::keyboard_event>();
	BOOST_REQUIRE(k);
	BOOST_TEST((k->key == KEYBOARD_BUTTONS::W));
	BOOST_TEST((k->action == KEY_ACTION::DOWN));
	BOOST_TEST((e.get_payload<core::inp::mouse_click_event>() == nullptr)); // other type -> null
	BOOST_TEST((e.get_payload<core::inp::cursor_move_event>() == nullptr));
}

BOOST_AUTO_TEST_CASE(Event_DefaultHasNoPayload)
{
	inp::input_event e;
	BOOST_TEST((e.get_payload<core::inp::keyboard_event>() == nullptr));
	BOOST_TEST(e.get_payload_type_id() == 0u);
}

BOOST_AUTO_TEST_CASE(Event_CopyAndMoveCarryPayloadAndDevice)
{
	auto original = move_event({ 3, 4 });
	original.get_device_id() = inp::device_id{ 2, 7 };

	inp::input_event copy(original);
	BOOST_REQUIRE(copy.get_payload<core::inp::cursor_move_event>());
	BOOST_TEST((copy.get_payload<core::inp::cursor_move_event>()->pos == glm::vec2{ 3, 4 }));
	BOOST_TEST((copy.get_device_id() == inp::device_id{ 2, 7 }));
	BOOST_TEST(copy.get_payload_type_id() == original.get_payload_type_id());
	BOOST_TEST((original.get_payload<core::inp::cursor_move_event>() != nullptr)); // copy leaves the source intact

	inp::input_event moved(std::move(original));
	BOOST_REQUIRE(moved.get_payload<core::inp::cursor_move_event>());
	BOOST_TEST((moved.get_payload<core::inp::cursor_move_event>()->pos == glm::vec2{ 3, 4 }));
	BOOST_TEST((moved.get_device_id() == inp::device_id{ 2, 7 }));
}

BOOST_AUTO_TEST_CASE(Event_ConstructPayloadReplacesPreviousPayload)
{
	inp::input_event e;
	e.construct_payload<core::inp::keyboard_event>(core::inp::keyboard_event{ KEYBOARD_BUTTONS::A, KEY_ACTION::UP });
	e.construct_payload<core::inp::scroll_move_event>(core::inp::scroll_move_event{ glm::vec2{ 0, 1 } });
	BOOST_TEST((e.get_payload<core::inp::keyboard_event>() == nullptr));
	BOOST_REQUIRE(e.get_payload<core::inp::scroll_move_event>());
	BOOST_TEST((e.get_payload<core::inp::scroll_move_event>()->direction == glm::vec2{ 0, 1 }));
}

BOOST_AUTO_TEST_CASE(DeviceId_OrdersByTypeThenIndex)
{
	BOOST_TEST((inp::device_id{ 1, 5 } < inp::device_id{ 2, 0 }));
	BOOST_TEST((inp::device_id{ 1, 1 } < inp::device_id{ 1, 2 }));
	BOOST_TEST((inp::device_id{ 3, 3 } == inp::device_id{ 3, 3 }));
}

// ---------------------------------------------------------------- ecs_input_manager

BOOST_AUTO_TEST_CASE(EcsManager_TracksKeyboardPressAndRelease)
{
	inp::ecs_input_manager mgr;
	const auto& state = mgr.raw();

	mgr.on_handle_event(nullptr, key_event(KEYBOARD_BUTTONS::W, KEY_ACTION::DOWN));
	BOOST_TEST(state.keyboard[idx(KEYBOARD_BUTTONS::W)]);
	BOOST_TEST(!state.keyboard[idx(KEYBOARD_BUTTONS::S)]);

	mgr.on_handle_event(nullptr, key_event(KEYBOARD_BUTTONS::W, KEY_ACTION::UP));
	BOOST_TEST(!state.keyboard[idx(KEYBOARD_BUTTONS::W)]);
}

BOOST_AUTO_TEST_CASE(EcsManager_TracksMouseButtons)
{
	inp::ecs_input_manager mgr;
	const auto& state = mgr.raw();
	mgr.on_handle_event(nullptr, click_event(MOUSE_BUTTONS::RIGHT, KEY_ACTION::DOWN));
	BOOST_TEST(state.mouse[idx(MOUSE_BUTTONS::RIGHT)]);
	BOOST_TEST(!state.mouse[idx(MOUSE_BUTTONS::LEFT)]);
}

BOOST_AUTO_TEST_CASE(EcsManager_NeverConsumesEvents)
{
	inp::ecs_input_manager mgr;
	BOOST_TEST(!mgr.on_handle_event(nullptr, key_event(KEYBOARD_BUTTONS::A, KEY_ACTION::DOWN)));
	BOOST_TEST(!mgr.on_handle_event(nullptr, move_event({ 1, 1 })));
}

BOOST_AUTO_TEST_CASE(EcsManager_CursorMoveKeepsTheRawPixelStep)
{
	inp::ecs_input_manager mgr;

	mgr.on_handle_event(nullptr, move_event({ 100, 50 }));
	mgr.on_handle_event(nullptr, move_event({ 110, 40 }));

	const auto& state = mgr.raw();
	BOOST_TEST((state.mouse_prev == glm::vec2{ 100, 50 }));
	BOOST_TEST((state.mouse_pos == glm::vec2{ 110, 40 }));
	// previous - current, in pixels: the router scales it by the size of the binding it routes through
	BOOST_TEST((state.mouse_dir == glm::vec2{ -10, 10 }));
}

BOOST_AUTO_TEST_CASE(EcsManager_KeepsRawStateWhereverTheCursorIs)
{
	inp::ecs_input_manager mgr;
	mgr.on_handle_event(nullptr, move_event({ 10, 10 }));
	mgr.on_handle_event(nullptr, key_event(KEYBOARD_BUTTONS::W, KEY_ACTION::DOWN));
	mgr.on_handle_event(nullptr, click_event(MOUSE_BUTTONS::LEFT, KEY_ACTION::DOWN));
	BOOST_TEST(mgr.raw().keyboard[idx(KEYBOARD_BUTTONS::W)]);
	BOOST_TEST(mgr.raw().mouse[idx(MOUSE_BUTTONS::LEFT)]);
}

BOOST_AUTO_TEST_CASE(EcsManager_ScrollIsLatchedUntilFrameDeltasReset)
{
	inp::ecs_input_manager mgr;
	const auto& state = mgr.raw();
	mgr.on_handle_event(nullptr, scroll_event({ 0, 3 }));
	BOOST_TEST((state.scroll == glm::vec2{ 0, 3 }));

	mgr.reset_frame_deltas();
	BOOST_TEST((state.scroll == glm::vec2{ 0 }));
	BOOST_TEST((state.mouse_dir == glm::vec2{ 0 }));
}

// ---------------------------------------------------------------- input_system routing

BOOST_AUTO_TEST_CASE(System_ConsumableListenerCanSwallowEventBeforeLayers)
{
	inp::input_system sys;
	std::vector<std::string> log;
	auto layer = std::make_shared<recording_layer>("game", false, log);
	sys.push_input_layer(win(1), { layer, false });

	auto eater = std::make_shared<recording_listener>(true);
	auto observer = std::make_shared<recording_listener>(false);
	sys.push_consumable_listener(observer);
	sys.push_consumable_listener(eater);

	sys.on_input_event(win(1), key_event(KEYBOARD_BUTTONS::A, KEY_ACTION::DOWN));
	BOOST_TEST(observer->calls == 1);
	BOOST_TEST(eater->calls == 1);
	BOOST_TEST(log.empty());

	sys.pop_consumable_listener(eater);
	sys.on_input_event(win(1), key_event(KEYBOARD_BUTTONS::A, KEY_ACTION::DOWN));
	BOOST_TEST(log.size() == 1u);
}

BOOST_AUTO_TEST_CASE(System_LayersReceiveEventsTopFirstAndBlockingLayerStopsPropagation)
{
	inp::input_system sys;
	std::vector<std::string> log;
	auto bottom = std::make_shared<recording_layer>("bottom", false, log);
	auto middle = std::make_shared<recording_layer>("middle", true, log);
	auto top = std::make_shared<recording_layer>("top", false, log);
	sys.push_input_layer(win(1), { bottom, false });
	sys.push_input_layer(win(1), { middle, true });   // handles AND blocks
	sys.push_input_layer(win(1), { top, false });

	sys.on_input_event(win(1), key_event(KEYBOARD_BUTTONS::A, KEY_ACTION::DOWN));
	BOOST_TEST((log == std::vector<std::string>{ "top", "middle" })); // bottom never sees it
}

BOOST_AUTO_TEST_CASE(System_HandledButNonBlockingLayerStillPropagates)
{
	inp::input_system sys;
	std::vector<std::string> log;
	sys.push_input_layer(win(1), { std::make_shared<recording_layer>("low", false, log), false });
	sys.push_input_layer(win(1), { std::make_shared<recording_layer>("high", true, log), false });
	sys.on_input_event(win(1), key_event(KEYBOARD_BUTTONS::A, KEY_ACTION::DOWN));
	BOOST_TEST((log == std::vector<std::string>{ "high", "low" }));
}

BOOST_AUTO_TEST_CASE(System_EventsAreRoutedPerWindow)
{
	inp::input_system sys;
	std::vector<std::string> log;
	sys.push_input_layer(win(1), { std::make_shared<recording_layer>("w1", false, log), false });
	sys.push_input_layer(win(2), { std::make_shared<recording_layer>("w2", false, log), false });
	sys.on_input_event(win(2), key_event(KEYBOARD_BUTTONS::A, KEY_ACTION::DOWN));
	BOOST_TEST((log == std::vector<std::string>{ "w2" }));
	BOOST_CHECK_NO_THROW(sys.on_input_event(win(3), key_event(KEYBOARD_BUTTONS::A, KEY_ACTION::DOWN))); // unknown window
}

BOOST_AUTO_TEST_CASE(System_NullWindowEventGoesToFocusedWindow)
{
	inp::input_system sys;
	std::vector<std::string> log;
	sys.push_input_layer(win(1), { std::make_shared<recording_layer>("w1", false, log), false });
	sys.push_input_layer(win(2), { std::make_shared<recording_layer>("w2", false, log), false });

	sys.on_input_event(nullptr, key_event(KEYBOARD_BUTTONS::A, KEY_ACTION::DOWN)); // nothing focused -> dropped
	BOOST_TEST(log.empty());

	sys.on_window_focus_gained(win(2));
	sys.on_input_event(nullptr, key_event(KEYBOARD_BUTTONS::A, KEY_ACTION::DOWN));
	BOOST_TEST((log == std::vector<std::string>{ "w2" }));

	sys.on_window_focus_lost(win(2));
	BOOST_TEST((sys.get_focused_window() == nullptr));
}

BOOST_AUTO_TEST_CASE(System_FirstCreatedWindowBecomesFocusedAndDestroyClearsIt)
{
	inp::input_system sys;
	sys.on_window_created(win(5));
	BOOST_TEST((sys.get_focused_window() == win(5)));
	sys.on_window_created(win(6));                       // does not steal focus
	BOOST_TEST((sys.get_focused_window() == win(5)));
	sys.on_window_destroyed(win(6));
	BOOST_TEST((sys.get_focused_window() == win(5)));
	sys.on_window_destroyed(win(5));
	BOOST_TEST((sys.get_focused_window() == nullptr));
}

BOOST_AUTO_TEST_CASE(System_PopLayerRemovesOnlyThatProcessor)
{
	inp::input_system sys;
	std::vector<std::string> log;
	auto a = std::make_shared<recording_layer>("a", false, log);
	auto b = std::make_shared<recording_layer>("b", false, log);
	sys.push_input_layer(win(1), { a, false });
	sys.push_input_layer(win(1), { b, false });
	sys.pop_input_layer(win(1), a);
	sys.on_input_event(win(1), key_event(KEYBOARD_BUTTONS::A, KEY_ACTION::DOWN));
	BOOST_TEST((log == std::vector<std::string>{ "b" }));
}

// ---------------------------------------------------------------- platform events (#103)

BOOST_AUTO_TEST_CASE(PlatformEvents_PayloadsRoundTrip)
{
	inp::input_event ch;
	ch.construct_payload<core::inp::char_event>(core::inp::char_event{ U'\U0001F600' });
	BOOST_REQUIRE(ch.get_payload<core::inp::char_event>());
	BOOST_TEST((ch.get_payload<core::inp::char_event>()->codepoint == U'\U0001F600'));
	BOOST_TEST((ch.get_payload<core::inp::framebuffer_resized>() == nullptr));

	inp::input_event fb;
	fb.construct_payload<core::inp::framebuffer_resized>(core::inp::framebuffer_resized{ glm::ivec2{ 3840, 2160 } });
	BOOST_REQUIRE(fb.get_payload<core::inp::framebuffer_resized>());
	BOOST_TEST((fb.get_payload<core::inp::framebuffer_resized>()->size == glm::ivec2{ 3840, 2160 }));

	inp::input_event sc;
	sc.construct_payload<core::inp::content_scale_changed>(core::inp::content_scale_changed{ glm::vec2{ 1.5f, 2.f } });
	inp::input_event copy(sc);
	BOOST_REQUIRE(copy.get_payload<core::inp::content_scale_changed>());
	BOOST_TEST((copy.get_payload<core::inp::content_scale_changed>()->scale == glm::vec2{ 1.5f, 2.f }));
}

BOOST_AUTO_TEST_CASE(PlatformEvents_AreDeliveredThroughInputSystem)
{
	struct capture_listener : inp::input_event_consumable_listener_interface
	{
		bool on_handle_event(wnd::handle, const inp::input_event& evt) override
		{
			if (auto* c = evt.get_payload<core::inp::char_event>()) chars.push_back(c->codepoint);
			if (auto* f = evt.get_payload<core::inp::framebuffer_resized>()) fb = f->size;
			if (auto* s = evt.get_payload<core::inp::content_scale_changed>()) scale = s->scale;
			return false;
		}
		std::vector<char32_t> chars;
		glm::ivec2 fb{ 0 };
		glm::vec2 scale{ 0.f };
	};

	inp::input_system sys;
	auto listener = std::make_shared<capture_listener>();
	sys.push_consumable_listener(listener);

	inp::input_event ch;
	ch.construct_payload<core::inp::char_event>(U'x');
	inp::input_event fb;
	fb.construct_payload<core::inp::framebuffer_resized>(glm::ivec2{ 800, 600 });
	inp::input_event sc;
	sc.construct_payload<core::inp::content_scale_changed>(glm::vec2{ 2.f, 2.f });

	sys.on_input_event(win(1), ch);
	sys.on_input_event(win(1), fb);
	sys.on_input_event(win(1), sc);

	BOOST_TEST((listener->chars == std::vector<char32_t>{ U'x' }));
	BOOST_TEST((listener->fb == glm::ivec2{ 800, 600 }));
	BOOST_TEST((listener->scale == glm::vec2{ 2.f, 2.f }));
}

BOOST_AUTO_TEST_SUITE_END()
