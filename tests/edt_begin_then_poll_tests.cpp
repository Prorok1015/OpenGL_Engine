#include <boost/test/unit_test.hpp>
#include "edt_begin_then_poll.h"
#include <snk/gui/gui_menu_layout.h>

// The editor's deferred start level (#428): begun once on the first frame, polled until it is done, although
// menu_layout_manager calls a copy of the callback every frame.

BOOST_AUTO_TEST_SUITE(edt_begin_then_poll_tests)

BOOST_AUTO_TEST_CASE(it_begins_once_and_polls_until_done_through_the_frame_copies)
{
	gui::menu_layout_manager menus;
	menus.set_show_main_menu(false);
	int begun = 0;
	int polled = 0;
	bool done = false;
	menus.register_implicit("load", edt::begin_then_poll([&] { ++begun; }, [&] { ++polled; return !done; }));

	for (int frame = 0; frame < 3; ++frame) {
		menus.process();
	}
	BOOST_TEST(begun == 1);
	BOOST_TEST(polled == 3);

	done = true;
	menus.process();
	menus.process();
	BOOST_TEST(begun == 1);
	BOOST_TEST(polled == 4);
}

// Live mode (#445) holds its builds while a module reload waits for the UI frame (live_build_deps::reload_pending).
BOOST_AUTO_TEST_CASE(a_pending_implicit_is_found_by_its_id_prefix_until_it_has_run)
{
	gui::menu_layout_manager menus;
	menus.set_show_main_menu(false);
	BOOST_TEST(!menus.has_implicit_prefix("edt/reload_game_module/"));
	menus.register_implicit("edt/reload_game_module/game", [] { return false; });
	BOOST_TEST(menus.has_implicit_prefix("edt/reload_game_module/"));
	BOOST_TEST(!menus.has_implicit_prefix("edt/live_reload_game_module/"));
	menus.process();
	BOOST_TEST(!menus.has_implicit_prefix("edt/reload_game_module/"));
}

BOOST_AUTO_TEST_SUITE_END()
