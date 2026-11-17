#include <boost/test/unit_test.hpp>
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_panel_registry.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/input/inp_input_router.h>
#include "edt_agent_confirmations.h"
#include "edt_agent_policy.h"
#include "edt_dockspace.h"
#include "edt_editor_layer.h"
#include "edt_editor_mode.h"
#include "edt_input_binding.h"
#include "test_temp_dir.hpp"
#include <snk/editor/edt_panel_base.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Game mode (#294) on a bare ImGui context: the dock layout outlives a stretch without the DockSpace, the full-window
// Game view binds no input under a popup, and the start level of --game is only a res:// tag. The loading screen of
// --game (#518) through edt::draw_layouts, the layout pass of editor_layer::on_update (which needs a whole
// editor_context): it replaces the dockspace and every shortcut, and the default layout is built after it.

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

	// A frame in which `key` goes down (and up again after it).
	template<typename F>
	void frame_with_key(ImGuiKey key, F&& body)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.AddKeyEvent(key, true);
		frame(body);
		io.AddKeyEvent(key, false);
		frame([] {});
	}

	ImGuiContext* ctx = nullptr;
};

struct empty_panel : edt::panel_base
{
	empty_panel() : panel_base("docked", "Docked") {}

protected:
	void on_render() override {}
};


// #470: the shortcuts of the editor and the two kinds of question that can wait for a person. One action on F5 stands for
// Ctrl+S and the others (any chord goes through the same pass); `modal_active` is what editor_system wires.
struct shortcut_rig
{
	shortcut_rig()
	{
		dialogs.set_registrar([](std::string_view, std::function<bool()>) {});
		edt::action_desc press;
		press.id = "t.press";
		press.label = "Press";
		press.menu_path = "File";
		press.shortcut = "F5";
		press.execute = edt::no_args([this] { ++presses; });
		registrations.push_back(actions.add(std::move(press)));
		layouts.modal_active = [this] { return dialogs.pending() > 0; };
	}

	// The shortcut pass and the dialogs in the order of the editor's frame, plus what `extra` draws (a window).
	void draw()
	{
		edt::draw_layouts(dock, panels, actions, layouts);
		if (extra) extra();
		dialogs.update();
	}
	void frame()
	{
		ui.frame([this] { draw(); });
	}
	void press(ImGuiKey key)
	{
		ui.frame_with_key(key, [this] { draw(); });
	}
	void ask_modal(int& answered, std::optional<std::size_t>& choice)
	{
		dialogs.choose("Question", "Really?", { "Yes", "No" }, [&](std::optional<std::size_t> picked) {
			choice = picked;
			++answered;
		});
	}

	ui_frames ui;
	edt::panel_registry panels;
	edt::action_registry actions;
	std::vector<edt::registration> registrations;
	edt::dockspace dock;
	edt::dialog_service dialogs;
	edt::editor_layouts layouts;
	std::function<void()> extra;
	int presses = 0;
};

// A real question of an agent (agent_confirmations), drawn by draw_agent_confirmations: an ordinary window.
struct agent_question
{
	agent_question()
	{
		cmd::command_desc desc;
		desc.id = "code.create";
		desc.category = cmd::agent_category::code_files;
		desc.summary = "does code.create";
		desc.args_schema = boost::json::value{ { "type", "object" } };
		desc.invoke = [](const cmd::call&) { return cmd::result::success(); };
		registrations.push_back(commands.add(std::move(desc)));
		gate->set_can_ask(true);
		gate->set_session_grants(&grants);
		commands.set_policy_gate(gate);
		confirmations = std::make_unique<edt::agent_confirmations>(commands, grants, [this] { return now; });
	}

	// Answers what still waits, into `finished`: it must go before the members it uses.
	~agent_question()
	{
		confirmations.reset();
	}

	void ask()
	{
		cmd::call request;
		request.args = boost::json::object{};
		request.from = cmd::origin::agent("s1");
		confirmations->invoke("code.create", std::move(request), [this](cmd::result outcome) { finished.push_back(std::move(outcome)); });
	}

	test_util::temp_dir dir{ "edt_modal_shortcuts" };
	cmd::command_registry commands;
	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	edt::session_grants grants;
	std::shared_ptr<edt::agent_policy_gate> gate = std::make_shared<edt::agent_policy_gate>(policy);
	std::chrono::steady_clock::time_point now{};
	std::vector<cmd::registration> registrations;
	std::vector<cmd::result> finished;
	std::unique_ptr<edt::agent_confirmations> confirmations;
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

BOOST_AUTO_TEST_CASE(TheLoadingScreen_ReplacesTheDockspaceAndTheShortcuts_AndTheDefaultLayoutComesAfterIt)
{
	ui_frames ui;
	edt::panel_registry panels;
	edt::action_registry actions;
	auto panel = std::make_shared<empty_panel>();
	auto registration = panels.add({ .id = "docked", .title = "Docked", .slot = edt::dock_slot::left, .open_by_default = true, .panel = panel });
	int toggles = 0;
	edt::action_desc toggle;
	toggle.id = "t.toggle";
	toggle.label = "Toggle";
	toggle.menu_path = "View";
	toggle.shortcut = "F11";
	toggle.execute = edt::no_args([&toggles] { ++toggles; });
	auto toggle_registration = actions.add(std::move(toggle));
	edt::dockspace dock;

	const std::string text = edt::loading_screen_text("res://levels/main.desc");
	BOOST_TEST(text == "Loading res://levels/main.desc...");
	bool loading = true;
	edt::editor_layouts layouts;
	layouts.loading_active = [&loading] { return loading; };
	layouts.draw_loading = [&text] { edt::draw_loading_screen(text); };
	const auto draw = [&](edt::drawn_layout expected) {
		BOOST_TEST((edt::draw_layouts(dock, panels, actions, layouts) == expected));
	};

	ui.frame([&] { draw(edt::drawn_layout::loading); });
	ui.frame_with_key(ImGuiKey_F11, [&] { draw(edt::drawn_layout::loading); });
	BOOST_TEST(toggles == 0);
	ui.frame([&] { draw(edt::drawn_layout::loading); });
	BOOST_TEST((ImGui::FindWindowByName("##dockspace_host") == nullptr)); // the menu bar is the host's
	BOOST_TEST((ImGui::FindWindowByName("Docked") == nullptr));
	ImGuiWindow* screen = ImGui::FindWindowByName(edt::loading_screen_window);
	BOOST_REQUIRE(screen);
	BOOST_TEST(screen->Active);
	BOOST_TEST((screen->Flags & ImGuiWindowFlags_NoTitleBar) != 0);
	BOOST_TEST((screen->Flags & ImGuiWindowFlags_NoBringToFrontOnFocus) != 0);
	BOOST_TEST((screen->Flags & ImGuiWindowFlags_NoDocking) != 0);
	BOOST_TEST(screen->Size.x == 1280.f);
	BOOST_TEST(screen->Size.y == 720.f);

	// The load ended: the editor layout with its default dock layout, built on the first render.
	loading = false;
	for (int i = 0; i < 3; ++i) ui.frame([&] { draw(edt::drawn_layout::editor); });
	BOOST_TEST(!screen->Active);
	BOOST_TEST((ImGui::FindWindowByName("##dockspace_host") != nullptr));
	ImGuiWindow* window = ImGui::FindWindowByName("Docked");
	BOOST_REQUIRE(window);
	BOOST_TEST(window->DockId != 0u);
	BOOST_TEST((window->DockNode != nullptr));
	ui.frame_with_key(ImGuiKey_F11, [&] { draw(edt::drawn_layout::editor); });
	BOOST_TEST(toggles == 1);

	// No loading screen set (an editor without --game): the editor layout as before.
	edt::editor_layouts plain;
	ui.frame([&] { BOOST_TEST((edt::draw_layouts(dock, panels, actions, plain) == edt::drawn_layout::editor)); });
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

BOOST_AUTO_TEST_CASE(Shortcuts_DoNotFireWhileADialogModalWaits_AndEscapeStillCancelsIt)
{
	shortcut_rig rig;
	rig.press(ImGuiKey_F5);
	BOOST_TEST(rig.presses == 1);

	int answered = 0;
	std::optional<std::size_t> choice = 0u;
	rig.ask_modal(answered, choice);
	BOOST_REQUIRE(rig.dialogs.pending() == 1u);
	rig.frame();
	rig.frame();
	rig.press(ImGuiKey_F5);
	BOOST_TEST(rig.presses == 1);
	BOOST_TEST(rig.dialogs.pending() == 1u);

	rig.press(ImGuiKey_Escape);
	BOOST_TEST(answered == 1);
	BOOST_TEST(!choice.has_value());
	BOOST_TEST(rig.dialogs.pending() == 0u);

	rig.press(ImGuiKey_F5);
	BOOST_TEST(rig.presses == 2);
}

BOOST_AUTO_TEST_CASE(Shortcuts_KeepWorkingUnderTheAgentQuestion_ButNotUnderAModalDialogAboveIt)
{
	shortcut_rig rig;
	agent_question agent;
	rig.extra = [&agent] { edt::draw_agent_confirmations(*agent.confirmations); };
	agent.ask();
	BOOST_REQUIRE(agent.confirmations->pending().size() == 1u);
	rig.frame();
	rig.frame();

	rig.press(ImGuiKey_F5);
	BOOST_TEST(rig.presses == 1);
	// Its window reads no key: Enter and Escape change nothing there, as before.
	rig.press(ImGuiKey_Enter);
	rig.press(ImGuiKey_Escape);
	BOOST_TEST(agent.confirmations->pending().size() == 1u);
	BOOST_TEST(agent.finished.empty());

	int answered = 0;
	std::optional<std::size_t> choice = 0u;
	rig.ask_modal(answered, choice);
	rig.frame();
	rig.frame();
	rig.press(ImGuiKey_F5);
	BOOST_TEST(rig.presses == 1);

	rig.press(ImGuiKey_Escape);
	BOOST_TEST(answered == 1);
	BOOST_TEST(!choice.has_value());
	BOOST_TEST(agent.confirmations->pending().size() == 1u);
	BOOST_TEST(agent.finished.empty());

	rig.press(ImGuiKey_F5);
	BOOST_TEST(rig.presses == 2);
}

BOOST_AUTO_TEST_SUITE_END()
