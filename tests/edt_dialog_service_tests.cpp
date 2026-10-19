#include <boost/test/unit_test.hpp>
#include "edt_file_menu.h"
#include <snk/command/cmd_json_schema.h>
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_service_commands.h>
#include <snk/window/wnd_null_window_system.h>
#include <boost/json/parse.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// dialog_service and the File menu flow on top of it, driven with auto answers: nothing here draws ImGui.
// Not covered headless: the ImGui drawing of each dialog and the dockspace wiring (attach).

namespace {

// Stands in for gui::menu_layout_manager: callbacks run every frame until they return false.
struct fake_gui
{
	void register_implicit(std::string_view id, std::function<bool()> draw)
	{
		ids.emplace_back(id);
		callbacks.emplace_back(std::string(id), std::move(draw));
	}

	void frame()
	{
		auto snapshot = callbacks;
		std::vector<std::string> done;
		for (auto& [id, draw] : snapshot) {
			if (!draw()) done.push_back(id);
		}
		std::erase_if(callbacks, [&done](const auto& c) {
			return std::find(done.begin(), done.end(), c.first) != done.end();
		});
	}

	std::vector<std::string> ids;
	std::vector<std::pair<std::string, std::function<bool()>>> callbacks;
};

// Headless ImGui context: enough to run popups without a GPU.
struct imgui_ctx
{
	imgui_ctx()
	{
		ctx = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800.f, 600.f);
		unsigned char* px = nullptr;
		int w = 0, h = 0;
		io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
	}
	~imgui_ctx() { ImGui::DestroyContext(ctx); }

	template<typename F>
	void frame(F&& body)
	{
		ImGui::NewFrame();
		body();
		ImGui::EndFrame();
	}

	ImGuiContext* ctx = nullptr;
};

struct dialog_fixture
{
	dialog_fixture()
	{
		service.set_registrar([this](std::string_view id, std::function<bool()> draw) {
			gui.register_implicit(id, std::move(draw));
		});
	}

	fake_gui gui;
	edt::dialog_service service;
	edt::action_registry actions;
};

struct menu_fixture : dialog_fixture
{
	menu_fixture()
	{
		hooks.has_scene = [this] { return has_scene; };
		hooks.is_dirty = [this] { return dirty; };
		hooks.level_path = [this] { return path; };
		hooks.new_level = [this] { calls.push_back("new"); };
		hooks.quick_save = [this] {
			calls.push_back("quick_save");
			if (quick_save_ok) dirty = false;
			return quick_save_ok;
		};
		hooks.save_level_as = [this](const std::string& p) {
			calls.push_back("save_as:" + p);
			if (save_ok) dirty = false;
			return save_ok;
		};
		hooks.discard_changes = [this] { calls.push_back("discard"); };
		hooks.load_level = [this](const std::filesystem::path& p) { calls.push_back("load:" + p.generic_string()); };
		hooks.import_model = [this](const std::filesystem::path& p) { calls.push_back("import:" + p.generic_string()); };
		hooks.exit = [this] { calls.push_back("exit"); };
		menu = std::make_unique<edt::file_menu>(service, actions, hooks);
		menu->register_actions();
	}

	void answer_unsaved(edt::unsaved_choice choice)
	{
		edt::dialog_auto_answer answer;
		answer.unsaved = choice;
		service.set_auto_answer(answer);
	}

	void drain()
	{
		for (int i = 0; i < 8 && !gui.callbacks.empty(); ++i) gui.frame();
	}

	bool has_scene = true;
	bool dirty = true;
	bool save_ok = true;
	bool quick_save_ok = true;
	std::string path;
	std::vector<std::string> calls;
	edt::file_menu_hooks hooks;
	std::unique_ptr<edt::file_menu> menu;
};

// A File menu wired to a window like the editor does: Exit shuts the window down, the window's own close
// request goes through the same menu.
struct closing_editor
{
	explicit closing_editor(menu_fixture& base)
		: window(windows.get_active_window())
		, menu(base.service, actions, exit_closes_window(base))
	{
		menu.register_actions();
		menu.attach_window_close(windows);
	}

	edt::file_menu_hooks exit_closes_window(menu_fixture& base)
	{
		edt::file_menu_hooks hooks = base.hooks;
		hooks.exit = [this, &base] {
			base.calls.push_back("exit");
			window->shutdown();
		};
		return hooks;
	}

	wnd::null_window_system windows;
	std::shared_ptr<wnd::window> window;
	edt::action_registry actions;
	edt::file_menu menu;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDialogServiceTests, dialog_fixture)

BOOST_AUTO_TEST_CASE(Ids_AreUniqueAcrossKindsAndCalls)
{
	std::set<edt::dialog_service::dialog_id> ids;
	ids.insert(service.confirm_unsaved({}));
	ids.insert(service.save_as("a", {}));
	ids.insert(service.pick_file({}, {}));
	ids.insert(service.message(edt::message_level::info, "x"));
	ids.insert(service.message(edt::message_level::error, "x"));
	BOOST_TEST(ids.size() == 5u);
}

BOOST_AUTO_TEST_CASE(Queue_ShowsOneDialogPerFrameInOrder)
{
	edt::dialog_auto_answer answer;
	answer.unsaved = edt::unsaved_choice::discard;
	service.set_auto_answer(answer);

	std::vector<std::string> order;
	const auto first = service.confirm_unsaved([&](edt::unsaved_choice) { order.push_back("first"); });
	const auto second = service.confirm_unsaved([&](edt::unsaved_choice) { order.push_back("second"); });

	BOOST_TEST(service.pending() == 2u);
	BOOST_REQUIRE(service.current().has_value());
	BOOST_TEST(*service.current() == first);
	BOOST_TEST(gui.ids.size() == 1u); // one pump for the whole queue

	gui.frame();
	BOOST_TEST((order == std::vector<std::string>{ "first" }));
	BOOST_TEST(service.pending() == 1u);
	BOOST_REQUIRE(service.current().has_value());
	BOOST_TEST(*service.current() == second);

	gui.frame();
	BOOST_TEST((order == std::vector<std::string>{ "first", "second" }));
	BOOST_TEST(service.pending() == 0u);
	BOOST_TEST(!service.current().has_value());
	BOOST_TEST(gui.callbacks.empty()); // the pump unregistered itself
}

BOOST_AUTO_TEST_CASE(Queue_DialogAskedFromAnAnswerRunsAfterTheOnesAlreadyWaiting)
{
	edt::dialog_auto_answer answer;
	answer.unsaved = edt::unsaved_choice::save;
	service.set_auto_answer(answer);

	std::vector<std::string> order;
	service.confirm_unsaved([&](edt::unsaved_choice) {
		order.push_back("a");
		service.confirm_unsaved([&](edt::unsaved_choice) { order.push_back("c"); });
	});
	service.confirm_unsaved([&](edt::unsaved_choice) { order.push_back("b"); });

	gui.frame();
	gui.frame();
	gui.frame();
	BOOST_TEST((order == std::vector<std::string>{ "a", "b", "c" }));
	BOOST_TEST(gui.callbacks.empty());
	BOOST_TEST(gui.ids.size() == 1u);
}

BOOST_AUTO_TEST_CASE(Queue_PumpIsRegisteredAgainUnderAFreshIdAfterItDrained)
{
	edt::dialog_auto_answer answer;
	answer.cancel_all = true;
	service.set_auto_answer(answer);

	service.message(edt::message_level::info, "one");
	gui.frame();
	BOOST_TEST(gui.callbacks.empty());

	service.message(edt::message_level::info, "two");
	BOOST_REQUIRE(gui.ids.size() == 2u);
	BOOST_TEST(gui.ids[0] != gui.ids[1]);
	BOOST_TEST(gui.callbacks.size() == 1u);
}

BOOST_AUTO_TEST_CASE(Queue_PumpStopsWhenTheServiceIsGone)
{
	auto owned = std::make_unique<edt::dialog_service>();
	fake_gui other;
	owned->set_registrar([&other](std::string_view id, std::function<bool()> draw) {
		other.register_implicit(id, std::move(draw));
	});
	owned->message(edt::message_level::info, "x");
	owned.reset();
	BOOST_REQUIRE(other.callbacks.size() == 1u);
	BOOST_TEST(!other.callbacks[0].second());
}

BOOST_AUTO_TEST_CASE(AutoAnswer_CoversEveryDialogKind)
{
	edt::dialog_auto_answer answer;
	answer.unsaved = edt::unsaved_choice::discard;
	answer.save_as_path = "levels/x.desc";
	answer.picked_file = std::filesystem::path("some/where.glb");
	service.set_auto_answer(answer);

	std::vector<std::string> got;
	service.confirm_unsaved([&](edt::unsaved_choice c) { got.push_back(c == edt::unsaved_choice::discard ? "discard" : "other"); });
	service.save_as("start", [&](const std::string& p) { got.push_back(p); });
	service.pick_file({}, [&](const std::filesystem::path& p) { got.push_back(p.generic_string()); });
	service.message(edt::message_level::warning, "careful");
	for (int i = 0; i < 4; ++i) gui.frame();

	BOOST_TEST((got == std::vector<std::string>{ "discard", "levels/x.desc", "some/where.glb" }));
	BOOST_TEST(service.pending() == 0u);
}

BOOST_AUTO_TEST_CASE(AutoAnswer_CancelAllReportsCancelOnlyForTheUnsavedPrompt)
{
	edt::dialog_auto_answer answer;
	answer.cancel_all = true;
	service.set_auto_answer(answer);

	std::vector<std::string> got;
	service.confirm_unsaved([&](edt::unsaved_choice c) { got.push_back(c == edt::unsaved_choice::cancel ? "cancel" : "other"); });
	service.save_as("start", [&](const std::string&) { got.push_back("path"); });
	service.pick_file({}, [&](const std::filesystem::path&) { got.push_back("file"); });
	for (int i = 0; i < 3; ++i) gui.frame();

	BOOST_TEST((got == std::vector<std::string>{ "cancel" }));
	BOOST_TEST(service.pending() == 0u);
}

// What Esc does to a modal: ImGui closes the popup, the request must not reopen it but resolve as cancelled.
BOOST_AUTO_TEST_CASE(Imgui_PopupClosedFromOutsideCancelsAndTheQueueAdvances)
{
	imgui_ctx imgui;
	std::vector<std::string> order;
	service.confirm_unsaved([&](edt::unsaved_choice c) {
		order.push_back(c == edt::unsaved_choice::cancel ? "cancel" : "other");
	});
	service.save_as("levels/x.desc", [&](const std::string&) { order.push_back("path"); });

	imgui.frame([&] { service.update(); });
	BOOST_TEST(order.empty()); // the first dialog is up and waiting
	BOOST_TEST(service.pending() == 2u);

	ImGui::ClosePopupToLevel(0, false);
	imgui.frame([&] { service.update(); });
	BOOST_TEST((order == std::vector<std::string>{ "cancel" }));
	BOOST_TEST(service.pending() == 1u);

	imgui.frame([&] { service.update(); }); // Save Level opens
	BOOST_TEST(service.pending() == 1u);
	ImGui::ClosePopupToLevel(0, false);
	imgui.frame([&] { service.update(); });
	BOOST_TEST((order == std::vector<std::string>{ "cancel" })); // cancelling a save-as reports nothing
	BOOST_TEST(service.pending() == 0u);
}

BOOST_AUTO_TEST_CASE(Imgui_FilePickerClosedFromOutsideCancelsAndANewOneOpensAgain)
{
	imgui_ctx imgui;
	int picked = 0;
	edt::pick_file_options options;
	options.title = "Pick";
	options.start_path = std::filesystem::current_path();
	service.pick_file(options, [&](const std::filesystem::path&) { ++picked; });

	imgui.frame([&] { service.update(); });
	BOOST_TEST(service.pending() == 1u); // on screen

	ImGui::ClosePopupToLevel(0, false); // Esc
	imgui.frame([&] { service.update(); });
	BOOST_TEST(service.pending() == 0u); // cancelled, not reopened
	BOOST_TEST(picked == 0);

	// A new picker must show up again, not resolve at once.
	service.pick_file(options, [&](const std::filesystem::path&) { ++picked; });
	imgui.frame([&] { service.update(); });
	BOOST_TEST(service.pending() == 1u);
	ImGui::ClosePopupToLevel(0, false);
	imgui.frame([&] { service.update(); });
	BOOST_TEST(service.pending() == 0u);
	BOOST_TEST(picked == 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(EdtFileMenuTests, menu_fixture)

BOOST_AUTO_TEST_CASE(New_WhenClean_RunsAtOnceWithoutAsking)
{
	dirty = false;
	menu->new_level();
	BOOST_TEST((calls == std::vector<std::string>{ "new" }));
	BOOST_TEST(service.pending() == 0u);
}

BOOST_AUTO_TEST_CASE(New_WhenDirty_AsksFirstAndDoesNothingUntilAnswered)
{
	menu->new_level();
	BOOST_TEST(calls.empty());
	BOOST_TEST(service.pending() == 1u);
}

BOOST_AUTO_TEST_CASE(New_WhenDirty_DiscardDropsChangesThenCreates)
{
	answer_unsaved(edt::unsaved_choice::discard);
	menu->new_level();
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "discard", "new" }));
}

BOOST_AUTO_TEST_CASE(New_WhenDirty_SaveOverwritesTheSavedLevelThenCreates)
{
	path = "levels/a.desc";
	answer_unsaved(edt::unsaved_choice::save);
	menu->new_level();
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "quick_save", "new" }));
}

BOOST_AUTO_TEST_CASE(New_WhenDirty_SaveOnAnUntitledLevelAsksForAPathFirst)
{
	edt::dialog_auto_answer answer;
	answer.unsaved = edt::unsaved_choice::save;
	answer.save_as_path = "levels/b.desc";
	service.set_auto_answer(answer);
	menu->new_level();
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "save_as:levels/b.desc", "new" }));
}

BOOST_AUTO_TEST_CASE(New_WhenDirty_FailedSaveAsDoesNotCreate)
{
	save_ok = false;
	edt::dialog_auto_answer answer;
	answer.unsaved = edt::unsaved_choice::save;
	answer.save_as_path = "levels/b.desc";
	service.set_auto_answer(answer);
	menu->new_level();
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "save_as:levels/b.desc" }));
}

BOOST_AUTO_TEST_CASE(New_WhenDirty_CancelChangesNothing)
{
	answer_unsaved(edt::unsaved_choice::cancel);
	menu->new_level();
	drain();
	BOOST_TEST(calls.empty());
	BOOST_TEST(service.pending() == 0u);
}

BOOST_AUTO_TEST_CASE(Exit_WhenClean_ExitsAtOnce)
{
	dirty = false;
	menu->exit();
	BOOST_TEST((calls == std::vector<std::string>{ "exit" }));
}

BOOST_AUTO_TEST_CASE(Exit_WhenDirty_ExitsOnlyAfterDiscardOrSave)
{
	answer_unsaved(edt::unsaved_choice::discard);
	menu->exit();
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "discard", "exit" }));

	calls.clear();
	path = "levels/a.desc";
	answer_unsaved(edt::unsaved_choice::save);
	menu->exit();
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "quick_save", "exit" }));
}

BOOST_AUTO_TEST_CASE(Exit_WhenDirty_CancelKeepsTheEditorOpen)
{
	answer_unsaved(edt::unsaved_choice::cancel);
	menu->exit();
	drain();
	BOOST_TEST(calls.empty());
}

BOOST_AUTO_TEST_CASE(Exit_WhenDirtyAndNothingLoaded_SaveDoesNotExit)
{
	has_scene = false;
	answer_unsaved(edt::unsaved_choice::save);
	menu->exit();
	drain();
	BOOST_TEST(calls.empty());
}

BOOST_AUTO_TEST_CASE(WindowClose_WhenDirty_AsksAndDoesNotExit)
{
	closing_editor editor(*this);
	editor.windows.request_close(*editor.window);
	BOOST_TEST(!editor.window->is_shutdown());
	BOOST_TEST(service.pending() == 1u);
	BOOST_TEST(calls.empty());
}

BOOST_AUTO_TEST_CASE(WindowClose_WhenDirty_CancelKeepsTheEditorRunning)
{
	closing_editor editor(*this);
	answer_unsaved(edt::unsaved_choice::cancel);
	editor.windows.request_close(*editor.window);
	drain();
	BOOST_TEST(!editor.window->is_shutdown());
	BOOST_TEST(!editor.windows.is_stop_running());
	BOOST_TEST(calls.empty());
	BOOST_TEST(service.pending() == 0u);
}

BOOST_AUTO_TEST_CASE(WindowClose_WhenDirty_DiscardClosesTheWindow)
{
	closing_editor editor(*this);
	answer_unsaved(edt::unsaved_choice::discard);
	editor.windows.request_close(*editor.window);
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "discard", "exit" }));
	BOOST_TEST(editor.window->is_shutdown());
}

BOOST_AUTO_TEST_CASE(WindowClose_WhenDirty_SaveWritesThenClosesTheWindow)
{
	path = "levels/a.desc";
	closing_editor editor(*this);
	answer_unsaved(edt::unsaved_choice::save);
	editor.windows.request_close(*editor.window);
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "quick_save", "exit" }));
	BOOST_TEST(editor.window->is_shutdown());
}

// The prompts the window close raises are ordinary dialog_service requests: an agent sees them with dialog.list and
// answers them with dialog.answer, and the editor goes on exactly as after a click.
BOOST_AUTO_TEST_CASE(WindowClose_WhenDirty_UnsavedPromptIsAnsweredThroughDialogAnswer)
{
	path = "levels/a.desc";
	closing_editor editor(*this);
	cmd::command_registry commands;
	auto registrations = edt::register_dialog_commands(commands, service);

	editor.windows.request_close(*editor.window);
	const cmd::result listed = commands.invoke("dialog.list", boost::json::object{});
	BOOST_REQUIRE_MESSAGE(listed.ok, listed.error);
	const boost::json::array& dialogs = listed.value.as_object().at("dialogs").as_array();
	BOOST_REQUIRE(dialogs.size() == 1u);
	BOOST_TEST(dialogs[0].as_object().at("kind").as_string().c_str() == std::string("unsaved"));
	const auto id = dialogs[0].as_object().at("id").as_int64();

	const cmd::result answered = commands.invoke("dialog.answer",
		boost::json::parse(std::format(R"({{"id":{},"answer":{{"choice":"save"}}}})", id)));
	BOOST_REQUIRE_MESSAGE(answered.ok, answered.error);
	BOOST_TEST(service.pending() == 0u);
	BOOST_TEST((calls == std::vector<std::string>{ "quick_save", "exit" }));
	BOOST_TEST(editor.window->is_shutdown());
}

BOOST_AUTO_TEST_CASE(WindowClose_WhenDirtyAndUntitled_SaveAsFollowsAndIsAnsweredThroughDialogAnswer)
{
	closing_editor editor(*this);
	cmd::command_registry commands;
	auto registrations = edt::register_dialog_commands(commands, service);

	editor.windows.request_close(*editor.window);
	auto first = service.list();
	BOOST_REQUIRE(first.size() == 1u);
	BOOST_TEST(first[0].kind == "unsaved");
	BOOST_TEST(service.answer(first[0].id, boost::json::parse(R"({"choice":"save"})")).ok);

	auto second = service.list(); // the untitled level asks for a path next
	BOOST_REQUIRE(second.size() == 1u);
	BOOST_TEST(second[0].kind == "save_as");
	const cmd::result answered = commands.invoke("dialog.answer",
		boost::json::parse(std::format(R"({{"id":{},"answer":{{"path":"levels/x.desc"}}}})", second[0].id)));
	BOOST_REQUIRE_MESSAGE(answered.ok, answered.error);
	BOOST_TEST(service.pending() == 0u);
	BOOST_TEST((calls == std::vector<std::string>{ "save_as:levels/x.desc", "exit" }));
	BOOST_TEST(editor.window->is_shutdown());
}

BOOST_AUTO_TEST_CASE(WindowClose_WhenClean_ClosesWithoutAsking)
{
	dirty = false;
	closing_editor editor(*this);
	editor.windows.request_close(*editor.window);
	BOOST_TEST(editor.window->is_shutdown());
	BOOST_TEST(service.pending() == 0u);
	BOOST_TEST((calls == std::vector<std::string>{ "exit" }));
}

BOOST_AUTO_TEST_CASE(WindowClose_WhenDirty_FailedSaveKeepsTheEditorOpen)
{
	path = "levels/a.desc";
	quick_save_ok = false;
	closing_editor editor(*this);
	answer_unsaved(edt::unsaved_choice::save);
	editor.windows.request_close(*editor.window);
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "quick_save" }));
	BOOST_TEST(!editor.window->is_shutdown());
}

BOOST_AUTO_TEST_CASE(New_WhenDirty_FailedSaveDoesNotCreate)
{
	path = "levels/a.desc";
	quick_save_ok = false;
	answer_unsaved(edt::unsaved_choice::save);
	menu->new_level();
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "quick_save" }));
}

BOOST_AUTO_TEST_CASE(WindowClose_WhileSaveAsIsOpen_AsksNothingMore)
{
	closing_editor editor(*this);
	editor.menu.save_level(); // Save As dialog on screen
	BOOST_TEST(service.pending() == 1u);
	editor.windows.request_close(*editor.window);
	BOOST_TEST(service.pending() == 1u);
	BOOST_TEST(!editor.window->is_shutdown());
}

BOOST_AUTO_TEST_CASE(WindowClose_AfterTheMenuIsGone_DoesNothing)
{
	wnd::null_window_system windows;
	auto window = windows.get_active_window();
	{
		edt::action_registry local_actions;
		edt::file_menu local(service, local_actions, hooks);
		local.attach_window_close(windows);
	}
	windows.request_close(*window);
	BOOST_TEST(service.pending() == 0u);
	BOOST_TEST(calls.empty());
}

BOOST_AUTO_TEST_CASE(WindowClose_RequestedAgainWhileThePromptIsUp_AsksOnce)
{
	closing_editor editor(*this);
	editor.windows.request_close(*editor.window);
	editor.windows.request_close(*editor.window);
	BOOST_TEST(service.pending() == 1u);
	BOOST_TEST(!editor.window->is_shutdown());
}

BOOST_AUTO_TEST_CASE(SaveLevel_AsksForAPathAndWritesIt)
{
	edt::dialog_auto_answer answer;
	answer.save_as_path = "levels/c.desc";
	service.set_auto_answer(answer);
	menu->save_level();
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "save_as:levels/c.desc" }));
}

BOOST_AUTO_TEST_CASE(SaveLevel_WithoutAScene_AsksNothing)
{
	has_scene = false;
	menu->save_level();
	BOOST_TEST(service.pending() == 0u);
	BOOST_TEST(calls.empty());
}

BOOST_AUTO_TEST_CASE(SaveLevel_RequestedTwiceOnAnUntitledLevel_OpensOneDialogAndWritesOnce)
{
	menu->save_level();
	menu->save_level();
	menu->quick_save(); // Ctrl+S while the dialog is open
	BOOST_TEST(service.pending() == 1u);

	edt::dialog_auto_answer answer;
	answer.save_as_path = "levels/c.desc";
	service.set_auto_answer(answer);
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "save_as:levels/c.desc" }));
}

BOOST_AUTO_TEST_CASE(New_SaveAndContinueWhileASaveAsIsOpen_JoinsItAndStillCreates)
{
	menu->new_level(); // unsaved prompt first
	menu->save_level(); // Ctrl+S: Save Level queued behind it
	BOOST_TEST(service.pending() == 2u);

	edt::dialog_auto_answer answer;
	answer.unsaved = edt::unsaved_choice::save;
	answer.save_as_path = "levels/c.desc";
	service.set_auto_answer(answer);
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "save_as:levels/c.desc", "new" }));
}

BOOST_AUTO_TEST_CASE(SaveAs_AfterACancelledOne_DoesNotRunItsLeftoverAction)
{
	edt::dialog_auto_answer cancel;
	cancel.unsaved = edt::unsaved_choice::save;
	cancel.cancel_all = true;
	service.set_auto_answer(cancel);
	menu->new_level();
	drain(); // Save & Continue -> Save As -> cancelled
	BOOST_TEST(calls.empty());

	edt::dialog_auto_answer answer;
	answer.save_as_path = "levels/c.desc";
	service.set_auto_answer(answer);
	menu->save_level();
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "save_as:levels/c.desc" }));
}

BOOST_AUTO_TEST_CASE(Dialogs_AnsweredAfterTheMenuIsGone_CallNothing)
{
	menu->new_level();
	menu->open_level();
	menu.reset();
	edt::dialog_auto_answer answer;
	answer.unsaved = edt::unsaved_choice::discard;
	answer.picked_file = std::filesystem::path("res/levels/e.desc");
	service.set_auto_answer(answer);
	drain();
	BOOST_TEST(calls.empty());
}

BOOST_AUTO_TEST_CASE(QuickSave_OverwritesATitledLevelAndAsksForAPathOtherwise)
{
	path = "levels/a.desc";
	menu->quick_save();
	BOOST_TEST((calls == std::vector<std::string>{ "quick_save" }));
	BOOST_TEST(service.pending() == 0u);

	calls.clear();
	path.clear();
	edt::dialog_auto_answer answer;
	answer.save_as_path = "levels/d.desc";
	service.set_auto_answer(answer);
	menu->quick_save();
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "save_as:levels/d.desc" }));
}

BOOST_AUTO_TEST_CASE(OpenLevelAndImportModel_HandOverThePickedFile)
{
	edt::dialog_auto_answer answer;
	answer.picked_file = std::filesystem::path("res/levels/e.desc");
	service.set_auto_answer(answer);
	menu->open_level();
	drain();
	answer.picked_file = std::filesystem::path("models/m.glb");
	service.set_auto_answer(answer);
	menu->import_model();
	drain();
	BOOST_TEST((calls == std::vector<std::string>{ "load:res/levels/e.desc", "import:models/m.glb" }));
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

namespace json = boost::json;

struct dialog_command_fixture : dialog_fixture
{
	dialog_command_fixture()
		: registrations(edt::register_dialog_commands(commands, service))
	{
	}

	cmd::result call(const std::string& id, const std::string& args = "{}")
	{
		return commands.invoke(id, json::parse(args), cmd::origin::test());
	}

	cmd::result answer(edt::dialog_service::dialog_id id, const std::string& answer_json)
	{
		return service.answer(id, json::parse(answer_json));
	}

	cmd::command_registry commands;
	std::vector<edt::registration> registrations;
};

bool any_popup_open()
{
	return ImGui::GetCurrentContext()->OpenPopupStack.Size > 0;
}

}

BOOST_FIXTURE_TEST_SUITE(EdtDialogAnswerTests, dialog_command_fixture)

BOOST_AUTO_TEST_CASE(List_ShowsIdKindTextAndAnswerSchemaOfEveryWaitingDialog)
{
	edt::pick_file_options options;
	options.title = "Pick a model";
	const auto unsaved = service.confirm_unsaved({});
	const auto save_as = service.save_as("levels/a.desc", {});
	const auto pick = service.pick_file(options, {});
	const auto message = service.message(edt::message_level::warning, "careful");

	const auto listed = service.list();
	BOOST_REQUIRE(listed.size() == 4u);
	BOOST_TEST(listed[0].id == unsaved);
	BOOST_TEST(listed[1].id == save_as);
	BOOST_TEST(listed[2].id == pick);
	BOOST_TEST(listed[3].id == message);
	BOOST_TEST(listed[0].kind == "unsaved");
	BOOST_TEST(listed[1].kind == "save_as");
	BOOST_TEST(listed[2].kind == "pick_file");
	BOOST_TEST(listed[3].kind == "message");
	BOOST_TEST(listed[2].text == "Pick a model");
	BOOST_TEST(listed[3].text == "careful");
	for (const auto& dialog : listed) {
		BOOST_TEST(!dialog.on_screen);
		BOOST_TEST(!cmd::json_schema::check_schema(dialog.answer_schema).has_value());
		BOOST_TEST(dialog.answer_schema.as_object().at("type").as_string().c_str() == std::string("object"));
	}
}

BOOST_AUTO_TEST_CASE(List_MarksTheDialogThatIsOnScreen)
{
	imgui_ctx imgui;
	service.message(edt::message_level::info, "one");
	service.message(edt::message_level::info, "two");
	imgui.frame([&] { service.update(); });

	const auto listed = service.list();
	BOOST_REQUIRE(listed.size() == 2u);
	BOOST_TEST(listed[0].on_screen);
	BOOST_TEST(!listed[1].on_screen);
}

BOOST_AUTO_TEST_CASE(DialogListCommand_HasTheDocumentedShape)
{
	BOOST_TEST(call("dialog.list").value.as_object().at("dialogs").as_array().empty());

	const auto id = service.confirm_unsaved({});
	const cmd::result listed = call("dialog.list");
	BOOST_REQUIRE_MESSAGE(listed.ok, listed.error);
	const json::array& dialogs = listed.value.as_object().at("dialogs").as_array();
	BOOST_REQUIRE(dialogs.size() == 1u);
	const json::object& dialog = dialogs[0].as_object();
	BOOST_TEST(dialog.at("id").as_int64() == static_cast<std::int64_t>(id));
	BOOST_TEST(dialog.at("kind").as_string().c_str() == std::string("unsaved"));
	BOOST_TEST(dialog.at("text").is_string());
	BOOST_TEST(dialog.at("answer_schema").is_object());
	BOOST_TEST(dialog.at("on_screen").is_bool());
	BOOST_TEST(dialog.size() == 5u);
}

BOOST_AUTO_TEST_CASE(Answer_OfADialogNeverDrawn_RunsTheCallbackAtOnce)
{
	std::vector<std::string> got;
	const auto unsaved = service.confirm_unsaved([&](edt::unsaved_choice c) {
		got.push_back(c == edt::unsaved_choice::discard ? "discard" : "other");
	});
	const auto save_as = service.save_as("start", [&](const std::string& p) { got.push_back("path:" + p); });
	const auto cancelled_save = service.save_as("start", [&](const std::string&) { got.push_back("never"); });
	const auto pick = service.pick_file({}, [&](const std::filesystem::path& p) { got.push_back("file:" + p.generic_string()); });
	const auto message = service.message(edt::message_level::info, "x");

	const cmd::result first = answer(unsaved, R"({"choice":"discard"})");
	BOOST_REQUIRE_MESSAGE(first.ok, first.error);
	BOOST_TEST(first.value.as_object().at("resolved").as_bool());
	BOOST_TEST(answer(save_as, R"({"path":"levels/x.desc"})").ok);
	BOOST_TEST(answer(cancelled_save, R"({"cancel":true})").ok);
	BOOST_TEST(answer(pick, R"({"path":"models/m.glb"})").ok);
	BOOST_TEST(answer(message, "{}").ok);

	BOOST_TEST((got == std::vector<std::string>{ "discard", "path:levels/x.desc", "file:models/m.glb" }));
	BOOST_TEST(service.pending() == 0u);
	BOOST_TEST(!service.is_pending(unsaved));
}

BOOST_AUTO_TEST_CASE(Answer_OfADialogOnScreen_ClosesItInTheNextFrameLikeAClick)
{
	imgui_ctx imgui;
	std::vector<std::string> got;
	const auto id = service.confirm_unsaved([&](edt::unsaved_choice c) {
		got.push_back(c == edt::unsaved_choice::save ? "save" : "other");
	});
	service.message(edt::message_level::info, "next");

	imgui.frame([&] { service.update(); });
	BOOST_TEST(any_popup_open());
	BOOST_TEST(got.empty());

	const cmd::result result = answer(id, R"({"choice":"save"})");
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_TEST(!result.value.as_object().at("resolved").as_bool()); // the frame resolves it
	BOOST_TEST(got.empty());
	BOOST_TEST(service.pending() == 2u);

	imgui.frame([&] { service.update(); });
	BOOST_TEST((got == std::vector<std::string>{ "save" }));
	BOOST_TEST(!any_popup_open()); // the popup was closed inside its own scope
	BOOST_TEST(service.pending() == 1u);
	BOOST_REQUIRE(service.current().has_value());
	BOOST_TEST(*service.current() != id);
}

BOOST_AUTO_TEST_CASE(Answer_OnScreen_SaveAsAndPickFile_GiveTheirCallbacksTheAnsweredPath)
{
	imgui_ctx imgui;
	std::vector<std::string> got;
	const auto save_as = service.save_as("start", [&](const std::string& p) { got.push_back("path:" + p); });
	imgui.frame([&] { service.update(); });
	BOOST_TEST(answer(save_as, R"({"path":"levels/long_enough_to_not_matter.desc"})").ok);
	imgui.frame([&] { service.update(); });
	BOOST_TEST((got == std::vector<std::string>{ "path:levels/long_enough_to_not_matter.desc" }));
	BOOST_TEST(!any_popup_open());

	edt::pick_file_options options;
	options.title = "Pick";
	options.start_path = std::filesystem::current_path();
	const auto pick = service.pick_file(options, [&](const std::filesystem::path& p) { got.push_back("file:" + p.generic_string()); });
	imgui.frame([&] { service.update(); });
	BOOST_TEST(any_popup_open());
	BOOST_TEST(answer(pick, R"({"path":"models/m.glb"})").ok);
	imgui.frame([&] { service.update(); });
	BOOST_TEST((got == std::vector<std::string>{ "path:levels/long_enough_to_not_matter.desc", "file:models/m.glb" }));
	BOOST_TEST(!any_popup_open());
	BOOST_TEST(service.pending() == 0u);
}

BOOST_AUTO_TEST_CASE(Answer_OnScreen_CancelOfAFilePickerReportsNothingAndClosesIt)
{
	imgui_ctx imgui;
	int picked = 0;
	edt::pick_file_options options;
	options.title = "Pick";
	options.start_path = std::filesystem::current_path();
	const auto pick = service.pick_file(options, [&](const std::filesystem::path&) { ++picked; });
	imgui.frame([&] { service.update(); });
	BOOST_TEST(answer(pick, R"({"cancel":true})").ok);
	imgui.frame([&] { service.update(); });
	BOOST_TEST(picked == 0);
	BOOST_TEST(service.pending() == 0u);
	BOOST_TEST(!any_popup_open());
}

BOOST_AUTO_TEST_CASE(Answer_OnScreen_AMessageIsAcknowledged)
{
	imgui_ctx imgui;
	const auto id = service.message(edt::message_level::error, "failed");
	imgui.frame([&] { service.update(); });
	BOOST_TEST(any_popup_open());
	BOOST_TEST(answer(id, "{}").ok);
	imgui.frame([&] { service.update(); });
	BOOST_TEST(service.pending() == 0u);
	BOOST_TEST(!any_popup_open());
}

BOOST_AUTO_TEST_CASE(Answer_ADialogBehindTheOneOnScreen_ResolvesWithoutDisturbingIt)
{
	imgui_ctx imgui;
	int first_calls = 0;
	std::vector<std::string> got;
	const auto first = service.confirm_unsaved([&](edt::unsaved_choice) { ++first_calls; });
	const auto second = service.confirm_unsaved([&](edt::unsaved_choice c) {
		got.push_back(c == edt::unsaved_choice::cancel ? "cancel" : "other");
	});
	imgui.frame([&] { service.update(); });

	BOOST_TEST(answer(second, R"({"choice":"cancel"})").value.as_object().at("resolved").as_bool());
	BOOST_TEST((got == std::vector<std::string>{ "cancel" }));
	BOOST_TEST(first_calls == 0);
	BOOST_TEST(service.is_pending(first));
	BOOST_TEST(!service.is_pending(second));
	BOOST_TEST(any_popup_open());
}

BOOST_AUTO_TEST_CASE(Answer_ThatBreaksTheSchemaOrMeansNothing_IsRejectedAndTheDialogStays)
{
	int calls = 0;
	const auto unsaved = service.confirm_unsaved([&](edt::unsaved_choice) { ++calls; });
	const auto save_as = service.save_as("start", [&](const std::string&) { ++calls; });
	const auto message = service.message(edt::message_level::info, "x");

	BOOST_TEST(answer(unsaved, R"({"choice":"maybe"})").code == "invalid_args");
	BOOST_TEST(answer(unsaved, "{}").code == "invalid_args");
	BOOST_TEST(answer(unsaved, R"({"choice":"save","extra":1})").code == "invalid_args");
	BOOST_TEST(answer(unsaved, R"({"choice":3})").code == "invalid_args");
	BOOST_TEST(service.answer(unsaved, json::value("save")).code == "invalid_args"); // not an object
	BOOST_TEST(answer(save_as, "{}").code == "invalid_args");
	BOOST_TEST(answer(save_as, R"({"path":""})").code == "invalid_args");
	BOOST_TEST(answer(save_as, R"({"path":"a","cancel":true})").code == "invalid_args");
	BOOST_TEST(answer(save_as, R"({"cancel":false})").code == "invalid_args");
	BOOST_TEST(answer(message, R"({"anything":1})").code == "invalid_args");

	BOOST_TEST(calls == 0);
	BOOST_TEST(service.pending() == 3u);
	const cmd::result bad = answer(unsaved, R"({"choice":"maybe"})");
	BOOST_TEST(bad.error.find("choice") != std::string::npos);

	BOOST_TEST(answer(unsaved, R"({"choice":"cancel"})").ok); // a valid one still works afterwards
	BOOST_TEST(calls == 1);
}

// Esc (or any user action) in the frame an answer lands in wins over the answer, for every kind of dialog.
BOOST_AUTO_TEST_CASE(Answer_AndEscInTheSameFrame_TheUserWins)
{
	imgui_ctx imgui;
	std::vector<std::string> got;
	edt::pick_file_options options;
	options.title = "Pick";
	options.start_path = std::filesystem::current_path();

	const auto unsaved = service.confirm_unsaved([&](edt::unsaved_choice c) {
		got.push_back(c == edt::unsaved_choice::cancel ? "cancel" : "other");
	});
	imgui.frame([&] { service.update(); });
	BOOST_TEST(answer(unsaved, R"({"choice":"save"})").ok);
	ImGui::ClosePopupToLevel(0, false);
	imgui.frame([&] { service.update(); });
	BOOST_TEST((got == std::vector<std::string>{ "cancel" }));
	BOOST_TEST(service.pending() == 0u);

	const auto save_as = service.save_as("start", [&](const std::string&) { got.push_back("save_as"); });
	imgui.frame([&] { service.update(); });
	BOOST_TEST(answer(save_as, R"({"path":"levels/x.desc"})").ok);
	ImGui::ClosePopupToLevel(0, false);
	imgui.frame([&] { service.update(); });
	BOOST_TEST(service.pending() == 0u);

	const auto pick = service.pick_file(options, [&](const std::filesystem::path&) { got.push_back("pick"); });
	imgui.frame([&] { service.update(); });
	BOOST_TEST(answer(pick, R"({"path":"models/m.glb"})").ok);
	ImGui::ClosePopupToLevel(0, false);
	imgui.frame([&] { service.update(); });
	BOOST_TEST(service.pending() == 0u);
	BOOST_TEST((got == std::vector<std::string>{ "cancel" })); // neither path dialog reported anything
}

BOOST_AUTO_TEST_CASE(Answer_UnknownOrAlreadyAnswered)
{
	imgui_ctx imgui;
	BOOST_TEST(answer(12345, "{}").code == "not_found");

	const auto id = service.confirm_unsaved({});
	imgui.frame([&] { service.update(); });
	BOOST_TEST(answer(id, R"({"choice":"cancel"})").ok);
	BOOST_TEST(answer(id, R"({"choice":"save"})").code == "failed"); // waiting for the frame that closes it
	imgui.frame([&] { service.update(); });
	BOOST_TEST(answer(id, R"({"choice":"save"})").code == "not_found");
}

BOOST_AUTO_TEST_CASE(DialogAnswerCommand_GoesThroughTheSameAnswer)
{
	int choice_seen = -1;
	const auto id = service.confirm_unsaved([&](edt::unsaved_choice c) { choice_seen = static_cast<int>(c); });
	const cmd::result ok = call("dialog.answer", std::format(R"({{"id":{},"answer":{{"choice":"discard"}}}})", id));
	BOOST_REQUIRE_MESSAGE(ok.ok, ok.error);
	BOOST_TEST(ok.value.as_object().at("id").as_int64() == static_cast<std::int64_t>(id));
	BOOST_TEST(ok.value.as_object().at("resolved").as_bool());
	BOOST_TEST(choice_seen == static_cast<int>(edt::unsaved_choice::discard));

	const auto other = service.confirm_unsaved({});
	BOOST_TEST(call("dialog.answer", std::format(R"({{"id":{},"answer":{{"choice":"nope"}}}})", other)).code == "invalid_args");
	BOOST_TEST(call("dialog.answer", R"({"id":99999,"answer":{}})").code == "not_found");
	BOOST_TEST(call("dialog.answer", R"({"id":1})").code == "invalid_args");
	BOOST_TEST(call("dialog.answer", R"({"id":1,"answer":5})").code == "invalid_args");
	BOOST_TEST(service.is_pending(other));
}

// An answer decides what a click on Save / Discard does, so it is a mutating command that asks for confirmation.
BOOST_AUTO_TEST_CASE(DialogCommands_AnswerIsMutatingAndNeedsConfirmation_ListIsReadOnly)
{
	const cmd::command_desc* answer_desc = commands.find("dialog.answer");
	const cmd::command_desc* list_desc = commands.find("dialog.list");
	BOOST_REQUIRE(answer_desc != nullptr);
	BOOST_REQUIRE(list_desc != nullptr);
	BOOST_TEST(answer_desc->mutates);
	BOOST_TEST((answer_desc->risk == cmd::danger::confirm));
	BOOST_TEST(!list_desc->mutates);
	BOOST_TEST((list_desc->risk == cmd::danger::none));
}

// The answer schema of a dialog that asks for a path must not accept {}: one of path / cancel is required.
BOOST_AUTO_TEST_CASE(AnswerSchemas_OfPathDialogsRequireSomething)
{
	const auto save_as = service.save_as("start", {});
	const auto pick = service.pick_file({}, {});
	for (const auto& dialog : service.list()) {
		if (dialog.id != save_as && dialog.id != pick) continue;
		BOOST_TEST(cmd::json_schema::validate(dialog.answer_schema, json::parse("{}")).has_value());
		BOOST_TEST(!cmd::json_schema::validate(dialog.answer_schema, json::parse(R"({"path":"a"})")).has_value());
		BOOST_TEST(!cmd::json_schema::validate(dialog.answer_schema, json::parse(R"({"cancel":true})")).has_value());
	}
}

// Python's json writes 3.0 for an integer; the schema accepts it, so dialog.answer must read it instead of throwing.
BOOST_AUTO_TEST_CASE(DialogAnswerCommand_AcceptsAnIntegralDoubleId)
{
	int got = 0;
	const auto id = service.confirm_unsaved([&](edt::unsaved_choice) { ++got; });
	cmd::result result;
	BOOST_CHECK_NO_THROW(result = call("dialog.answer", std::format(R"({{"id":{}.0,"answer":{{"choice":"cancel"}}}})", id)));
	BOOST_TEST(result.ok);
	BOOST_TEST(got == 1);
	BOOST_TEST(call("dialog.answer", R"({"id":1e30,"answer":{}})").code == "invalid_args");
}

BOOST_AUTO_TEST_CASE(AutoAnswer_StillWinsOverTheQueue_AndAnswerWorksForWhatItDoesNotCover)
{
	edt::dialog_auto_answer auto_answer;
	auto_answer.unsaved = edt::unsaved_choice::discard;
	service.set_auto_answer(auto_answer);

	std::vector<std::string> got;
	service.confirm_unsaved([&](edt::unsaved_choice) { got.push_back("auto"); });
	const auto uncovered = service.save_as("start", [&](const std::string& p) { got.push_back("path:" + p); });
	gui.frame(); // the unsaved prompt is answered automatically
	BOOST_TEST((got == std::vector<std::string>{ "auto" }));
	BOOST_TEST(answer(uncovered, R"({"path":"levels/z.desc"})").ok);
	BOOST_TEST((got == std::vector<std::string>{ "auto", "path:levels/z.desc" }));
}

BOOST_AUTO_TEST_SUITE_END()
