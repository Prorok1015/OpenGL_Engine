#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/input/inp_input_router.h>
#include "edt_dockspace.h"
#include "edt_editor_mode.h"
#include "edt_input_binding.h"
#include "edt_panel_base.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <optional>
#include <string>

// Game mode (#294) on a bare ImGui context: the dock layout outlives a stretch without the DockSpace, the full-window
// Game view binds no input under a popup, and the start level of --game is only a res:// tag.

namespace {

struct ui_frames
{
	ui_frames()
	{
		ctx = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		io.DisplaySize = ImVec2(1280.f, 720.f);
		unsigned char* px = nullptr;
		int w = 0, h = 0;
		io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
	}
	~ui_frames() { ImGui::DestroyContext(ctx); }
	ui_frames(const ui_frames&) = delete;
	ui_frames& operator=(const ui_frames&) = delete;

	template<typename F>
	void frame(F&& body)
	{
		ImGui::NewFrame();
		body();
		ImGui::EndFrame();
	}

	ImGuiContext* ctx = nullptr;
};

struct empty_panel : edt::panel_base
{
	empty_panel() : panel_base("docked", "Docked") {}

protected:
	void on_render() override {}
};

}

BOOST_AUTO_TEST_SUITE(EdtGameModeUiTests)

BOOST_AUTO_TEST_CASE(TheDockLayout_SurvivesFramesOfTheGameLayout)
{
	ui_frames ui;
	edt::panel_registry panels;
	edt::action_registry actions;
	auto panel = std::make_shared<empty_panel>();
	auto registration = panels.add({ .id = "docked", .title = "Docked", .slot = edt::dock_slot::left, .open_by_default = true, .panel = panel });
	edt::dockspace dock;

	for (int i = 0; i < 3; ++i) ui.frame([&] { dock.render(panels, actions); });
	ImGuiWindow* window = ImGui::FindWindowByName("Docked");
	BOOST_REQUIRE(window);
	const ImGuiID node_id = window->DockId;
	BOOST_REQUIRE(node_id != 0u);
	BOOST_REQUIRE(window->DockNode);

	// Game mode: no DockSpace, no panels, one undocked window over everything.
	for (int i = 0; i < 5; ++i) {
		ui.frame([&] {
			dock.keep_alive();
			ImGui::Begin("##game_mode_view", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking);
			ImGui::End();
		});
	}
	BOOST_TEST((ImGui::DockBuilderGetNode(node_id) != nullptr));

	for (int i = 0; i < 2; ++i) ui.frame([&] { dock.render(panels, actions); });
	BOOST_TEST(window->DockId == node_id);
	BOOST_TEST((window->DockNode == ImGui::DockBuilderGetNode(node_id)));
}

BOOST_AUTO_TEST_CASE(TheGameWindow_BindsNoInputUnderAPopupOrADialog)
{
	BOOST_TEST(edt::game_input_allowed(false, false));
	BOOST_TEST(!edt::game_input_allowed(true, false));
	BOOST_TEST(!edt::game_input_allowed(false, true));

	ui_frames ui;
	inp::input_router router{ std::make_shared<inp::ecs_input_manager>() };
	const std::optional<inp::input_target> play{ inp::input_target{ .set = "play", .context = "w0" } };
	const auto bind = [&](bool dialog_open) {
		router.clear_bindings();
		return edt::bind_game_window_input(&router, play, { 0.f, 0.f }, { 1280.f, 720.f }, dialog_open);
	};

	ui.frame([&] { BOOST_TEST(bind(false)); });
	BOOST_TEST(router.binding_count() == 1u);
	ui.frame([&] { BOOST_TEST(!bind(true)); });
	BOOST_TEST(router.binding_count() == 0u);

	ui.frame([&] {
		ImGui::Begin("host");
		ImGui::OpenPopup("question");
		if (ImGui::BeginPopup("question")) ImGui::EndPopup();
		ImGui::End();
		BOOST_TEST(!bind(false));
	});
	BOOST_TEST(router.binding_count() == 0u);
	ui.frame([&] {
		BOOST_TEST(!bind(false));
		ImGui::Begin("host");
		if (ImGui::BeginPopup("question")) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		ImGui::End();
		BOOST_TEST(bind(false));
	});
	BOOST_TEST(router.binding_count() == 1u);
}

BOOST_AUTO_TEST_CASE(TheGameLevel_IsOnlyAPortableResTag)
{
	BOOST_TEST(edt::game_level_problem("res://levels/main.desc").empty());
	BOOST_TEST(edt::game_level_problem("res://main.desc").empty());
	for (const char* refused : { "levels/main.desc", "/abs/main.desc", "C:\\levels\\main.desc", "C:/levels/main.desc",
			"file:///abs/main.desc", "memory://editor/main.desc", "user://main.desc", "res://", "res:///abs/main.desc",
			"res://../main.desc", "res://levels/../../main.desc", "res://levels\\main.desc", "res://levels//main.desc",
			"res://c:/main.desc", "RES://levels/main.desc", "../main.desc" }) {
		BOOST_TEST(!edt::game_level_problem(refused).empty(), refused);
	}
}

BOOST_AUTO_TEST_SUITE_END()
