#include <boost/test/unit_test.hpp>
#include "wnd_null_window_system.h"
#include "ds/ds_store.hpp"
#include <memory>

namespace {

struct counting_listener final : wnd::window_listener_interface
{
	int created = 0;

	void on_input_event(wnd::handle, const inp::input_event&) override {}
	void on_window_focus_gained(wnd::handle) override {}
	void on_window_focus_lost(wnd::handle) override {}
	void on_window_resize(wnd::handle, int, int) override {}
	void on_window_moved(wnd::handle, int, int) override {}
	void on_window_close(wnd::handle) override {}
	void on_window_refresh(wnd::handle) override {}
	void on_window_minimized(wnd::handle) override {}
	void on_window_restored(wnd::handle) override {}
	void on_window_maximized(wnd::handle) override {}
	void on_window_created(wnd::handle) override { ++created; }
	void on_window_destroyed(wnd::handle) override {}
};

}

BOOST_AUTO_TEST_SUITE(WndNullWindowSystemTests)

BOOST_AUTO_TEST_CASE(HasActiveWindowWithoutDisplayOrContext)
{
	wnd::null_window_system sys;
	auto win = sys.get_active_window();

	BOOST_REQUIRE(win);
	BOOST_TEST((win->get_size() == glm::ivec2{ 1920, 1080 }));
	BOOST_TEST((win->get_backbuffer_size() == win->get_size()));
	BOOST_TEST(!win->get_native_handle());
	BOOST_TEST(win->get_content_scale() == 1.f);
	BOOST_TEST(!win->gl_surface_ptr());
	BOOST_TEST(!sys.get_active_native_handle());
	BOOST_TEST(sys.get_active_content_scale() == 1.f);
}

BOOST_AUTO_TEST_CASE(FrameCallsAreNoOps)
{
	wnd::null_window_system sys;

	sys.init();
	sys.init_windows_frame_time();
	sys.pool_events();
	sys.process_windows();
	BOOST_TEST(!sys.is_stop_running());
}

BOOST_AUTO_TEST_CASE(ShutdownStopsRunning)
{
	wnd::null_window_system sys;
	auto win = sys.get_active_window();

	BOOST_TEST(!win->is_shutdown());
	win->shutdown();
	BOOST_TEST(win->is_shutdown());
	BOOST_TEST(sys.is_stop_running());

	win->shutdown(false);
	BOOST_TEST(!sys.is_stop_running());
}

BOOST_AUTO_TEST_CASE(WindowStoresTitleAndIgnoresEmpty)
{
	wnd::null_window_system sys;
	auto win = std::static_pointer_cast<wnd::null_window>(sys.get_active_window());

	win->set_title("Test window");
	BOOST_TEST(win->get_title() == "Test window");
	win->set_title("");
	BOOST_TEST(win->get_title() == "Test window");

	win->set_cursor_mode(wnd::CURSOR_MODE::HIDDEN);
	BOOST_TEST((win->get_cursor_mode() == wnd::CURSOR_MODE::HIDDEN));
}

BOOST_AUTO_TEST_CASE(ClipboardRoundTrip)
{
	wnd::null_window win;
	BOOST_TEST(win.get_clipboard_text().empty());
	win.set_clipboard_text("hello \xD0\xBC\xD0\xB8\xD1\x80");
	BOOST_TEST(win.get_clipboard_text() == "hello \xD0\xBC\xD0\xB8\xD1\x80");
	win.set_clipboard_text("");
	BOOST_TEST(win.get_clipboard_text().empty());
}

BOOST_AUTO_TEST_CASE(CursorShapeIsStored)
{
	wnd::null_window win;
	BOOST_TEST((win.get_cursor_shape() == wnd::cursor_shape::arrow));
	win.set_cursor_shape(wnd::cursor_shape::resize_nwse);
	BOOST_TEST((win.get_cursor_shape() == wnd::cursor_shape::resize_nwse));
}

BOOST_AUTO_TEST_CASE(AspectRatioFollowsSize)
{
	wnd::null_window win{ glm::ivec2{ 800, 400 } };
	BOOST_TEST(win.aspect_ratio() == 2.f);

	wnd::null_window minimized{ glm::ivec2{ 0, 0 } };
	BOOST_TEST(minimized.is_minimize_mode());
	BOOST_TEST(minimized.aspect_ratio() == 0.f);
}

BOOST_AUTO_TEST_CASE(ListenersAddAndRemove)
{
	wnd::null_window_system sys;
	auto listener = std::make_shared<counting_listener>();

	sys.add_event_listener(listener);
	BOOST_TEST(sys.get_event_listeners().size() == 1u);
	sys.remove_event_listener(listener);
	BOOST_TEST(sys.get_event_listeners().empty());
}

BOOST_AUTO_TEST_CASE(StorageExposesInterface)
{
	ds::app_data_storage storage;
	storage.construct<wnd::window_system, wnd::null_window_system>();

	auto& sys = storage.require<wnd::window_system>();
	BOOST_TEST(static_cast<bool>(sys.get_active_window()));
	storage.destruct<wnd::window_system>();
	BOOST_TEST(!storage.has_value<wnd::window_system>());
}

BOOST_AUTO_TEST_SUITE_END()
