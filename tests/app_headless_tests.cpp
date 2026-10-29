#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/gui/gui_null_backend.h>
#include <snk/gui/gui_platform_backend.h>
#include <snk/gui/gui_system.h>
#include <snk/window/wnd_window_service_init.h>
#include <imgui.h>
#include <string_view>

// The pieces of `editor --headless` (#452) that do not need the editor itself.

namespace {
	// Puts window/system back even when a REQUIRE fails, so later tests get the default window system.
	struct window_system_guard
	{
		explicit window_system_guard(std::string_view value) { cfg::set_variable("window/system", value); }
		~window_system_guard() { cfg::set_variable("window/system", "glfw"); }
	};
}

BOOST_AUTO_TEST_SUITE(AppHeadlessTests)

BOOST_AUTO_TEST_CASE(WindowSystemNullFromConfigBuildsAWindowWithoutAGlSurface)
{
	window_system_guard guard("null");
	ds::app_data_storage storage;
	core::window::window_init(storage);
	auto& windows = storage.require<wnd::window_system>();
	BOOST_TEST(dynamic_cast<wnd::null_window_system*>(&windows) != nullptr);
	BOOST_REQUIRE(windows.get_active_window());
	BOOST_TEST(windows.get_active_window()->gl_surface_ptr() == nullptr);
	BOOST_TEST(!windows.is_stop_running());
	core::window::window_term(storage);
}

BOOST_FIXTURE_TEST_CASE(ExitAfterFramesStopsTheEngineFrameLoop, test_util::null_engine_fixture)
{
	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));
	auto* loop = dynamic_cast<app::loop_service*>(&storage.require<app::app_loop_service_interface>());
	BOOST_REQUIRE(loop);
	loop->set_exit_after_frames(4);
	app::application application;
	BOOST_TEST(application.run(storage) == 0);
	BOOST_TEST(swapchain()->frames_presented() == 4u);
}

// The headless `ui` stage runs the editor's implicit callbacks (a level load shows a modal) inside an ImGui frame
// that nothing draws: no GL, only the null renderer half.
BOOST_FIXTURE_TEST_CASE(NullImguiBackendRunsAFrameWithoutDrawing, test_util::null_engine_fixture)
{
	auto* driver = storage.require<rnd::render_system>().get_driver();
	BOOST_REQUIRE(driver);
	auto window = storage.require<wnd::window_system>().get_active_window();
	BOOST_REQUIRE(window);

	gui::null_imgui_backend backend(*driver);
	backend.init();
	gui::platform_backend platform(*window);
	platform.init();
	{
		gui::gui_system gui(&backend, &platform);
		int ran = 0;
		for (int frame = 0; frame < 3; ++frame) {
			gui.run_without_drawing([&ran] {
				ImGui::OpenPopup("Loading##headless_test");
				if (ImGui::BeginPopupModal("Loading##headless_test", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
					ImGui::Text("Loading...");
					ImGui::EndPopup();
				}
				++ran;
			});
		}
		BOOST_TEST(ran == 3);
		BOOST_TEST(ImGui::GetFrameCount() == 3);
	}
	platform.shutdown();
	backend.shutdown();
	BOOST_TEST(ImGui::GetCurrentContext() == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
