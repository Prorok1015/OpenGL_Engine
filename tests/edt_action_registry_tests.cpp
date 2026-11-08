#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_action_registry.h>
#include "edt_file_menu.h"
#include "edt_main_menu.h"
#include "test_temp_dir.hpp"
#include "edt_editor_mode.h"
#include <snk/command/cmd_json_schema.h>
#include <imgui.h>
#include <algorithm>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <filesystem>
#include <functional>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// action_registry, the File menu registered through it and the built-in menu order, all without ImGui.
// Not covered headless: the ImGui drawing of the menu bar. The shortcut pass (fire_shortcuts) runs in a bare ImGui
// context (EdtShortcutPassTests).

namespace {

edt::action_desc make_action(std::string id, std::string label, std::string menu, std::function<void()> run,
	std::string shortcut_text = {})
{
	edt::action_desc desc;
	desc.id = std::move(id);
	desc.label = std::move(label);
	desc.menu_path = std::move(menu);
	desc.shortcut = std::move(shortcut_text);
	if (run) desc.execute = edt::no_args(std::move(run));
	return desc;
}

std::vector<std::string> labels_of(const std::vector<edt::menu_item_info>& items)
{
	std::vector<std::string> out;
	for (const auto& item : items) out.push_back(item.label);
	return out;
}

// Id of the File menu item that shows `shortcut_text`; empty if there is none.
std::string file_item_with_shortcut(edt::action_registry& actions, std::string_view shortcut_text)
{
	for (const auto& item : actions.items("File")) {
		if (item.shortcut_text == shortcut_text) return item.id;
	}
	return {};
}

edt::shortcut chord(std::string_view text)
{
	auto parsed = edt::shortcut::parse(text);
	BOOST_REQUIRE(parsed.has_value());
	return *parsed;
}

// Stands in for gui::menu_layout_manager: callbacks run every frame until they return false.
struct fake_gui
{
	void register_implicit(std::string_view id, std::function<bool()> draw)
	{
		callbacks.emplace_back(std::string(id), std::move(draw));
	}

	void drain()
	{
		for (int i = 0; i < 8 && !callbacks.empty(); ++i) {
			auto snapshot = callbacks;
			std::vector<std::string> done;
			for (auto& [id, draw] : snapshot) {
				if (!draw()) done.push_back(id);
			}
			std::erase_if(callbacks, [&done](const auto& c) {
				return std::find(done.begin(), done.end(), c.first) != done.end();
			});
		}
	}

	std::vector<std::pair<std::string, std::function<bool()>>> callbacks;
};

struct file_menu_fixture
{
	file_menu_fixture()
	{
		dialogs.set_registrar([this](std::string_view id, std::function<bool()> draw) {
			gui.register_implicit(id, std::move(draw));
		});
		hooks.has_scene = [this] { return has_scene; };
		hooks.is_dirty = [this] { return dirty; };
		hooks.level_path = [this] { return path; };
		hooks.new_level = [this] { calls.push_back("new"); };
		hooks.quick_save = [this] { calls.push_back("quick_save"); return true; };
		hooks.save_level_as = [this](const std::string& p) { calls.push_back("save_as:" + p); return save_ok; };
		hooks.discard_changes = [this] { dirty = false; };
		hooks.load_level = [this](const std::filesystem::path& p) { calls.push_back("load:" + p.generic_string()); };
		hooks.import_model = [this](const std::filesystem::path& p) { calls.push_back("import:" + p.generic_string()); };
		hooks.exit = [this] { calls.push_back("exit"); };
		menu = std::make_unique<edt::file_menu>(dialogs, actions, hooks);
		menu->register_actions();
	}

	fake_gui gui;
	edt::dialog_service dialogs;
	edt::action_registry actions;
	edt::file_menu_hooks hooks;
	std::unique_ptr<edt::file_menu> menu;
	bool dirty = false;
	bool has_scene = true;
	bool save_ok = true;
	std::string path = "levels/saved.desc";
	std::vector<std::string> calls;
};

}

BOOST_AUTO_TEST_SUITE(EdtShortcutTests)

BOOST_AUTO_TEST_CASE(Parse_ReadsModifiersAndKeyInAnyCase)
{
	const auto s = chord("ctrl+Shift+s");
	BOOST_CHECK(s.ctrl && s.shift && !s.alt);
	BOOST_CHECK_EQUAL(s.key, "S");
	BOOST_CHECK_EQUAL(s.to_string(), "Ctrl+Shift+S");
	BOOST_CHECK(chord("F5").key == "F5" && !chord("F5").ctrl);
	BOOST_CHECK(chord("Alt+Delete").alt);
}

BOOST_AUTO_TEST_CASE(Parse_AcceptsPunctuationKeysAndRoundTrips)
{
	for (const char* key : { "`", "[", "]", "\\", "'", ",", ".", "/", ";", "=", "-" }) {
		const auto s = chord(std::string("Ctrl+") + key);
		BOOST_CHECK(s.ctrl);
		BOOST_CHECK_EQUAL(s.key, key);
		BOOST_CHECK_EQUAL(s.to_string(), std::string("Ctrl+") + key);
		BOOST_CHECK(chord(s.to_string()) == s);
	}
}

BOOST_AUTO_TEST_CASE(Parse_RejectsMalformedText)
{
	for (const char* text : { "", "Ctrl", "Ctrl+", "+S", "Ctrl++S", "S+D", "Ctrl+S+" }) {
		BOOST_TEST(!edt::shortcut::parse(text).has_value(), text);
	}
}

BOOST_AUTO_TEST_CASE(Equality_ComparesModifiersAndKey)
{
	BOOST_CHECK(chord("Ctrl+S") == chord("ctrl+s"));
	BOOST_CHECK(chord("Ctrl+S") != chord("S"));
	BOOST_CHECK(chord("Ctrl+S") != chord("Ctrl+Shift+S"));
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

// A bare ImGui context, no window and no backend: enough for the key state fire_shortcuts reads.
struct shortcut_frames
{
	shortcut_frames()
	{
		ctx = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800.f, 600.f);
		unsigned char* px = nullptr;
		int w = 0, h = 0;
		io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
	}
	~shortcut_frames() { ImGui::DestroyContext(ctx); }
	shortcut_frames(const shortcut_frames&) = delete;
	shortcut_frames& operator=(const shortcut_frames&) = delete;

	// One frame in which `key` goes down (and up again after it); `text_input`: a text field has the keyboard.
	bool press(edt::action_registry& actions, ImGuiKey key, bool text_input, bool ctrl = false,
		const std::function<bool(std::string_view)>& allowed = {})
	{
		ImGuiIO& io = ImGui::GetIO();
		if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
		io.AddKeyEvent(key, true);
		ImGui::NewFrame();
		// NewFrame takes it from the previous frame's widgets; there are none here.
		io.WantTextInput = text_input;
		const bool fired = edt::fire_shortcuts(actions, allowed);
		ImGui::EndFrame();
		io.AddKeyEvent(key, false);
		if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
		ImGui::NewFrame();
		ImGui::EndFrame();
		return fired;
	}

	ImGuiContext* ctx = nullptr;
};

}

BOOST_AUTO_TEST_SUITE(EdtShortcutPassTests)

BOOST_AUTO_TEST_CASE(AShortcutThatYieldsToTextInput_DoesNotFireWhileTextIsTyped)
{
	shortcut_frames frames;
	edt::action_registry actions;
	int toggles = 0;
	int saves = 0;
	auto toggle = make_action("t.toggle", "Toggle", "View", [&] { ++toggles; }, "F11");
	toggle.yields_to_text_input = true;
	auto toggle_reg = actions.add(std::move(toggle));
	auto save_reg = actions.add(make_action("t.save", "Save", "File", [&] { ++saves; }, "F12"));

	BOOST_TEST(!frames.press(actions, ImGuiKey_F11, true));
	BOOST_TEST(toggles == 0);
	BOOST_TEST(frames.press(actions, ImGuiKey_F11, false));
	BOOST_TEST(toggles == 1);
	// One that does not yield fires while typing too, as it did from the menu bar.
	BOOST_TEST(frames.press(actions, ImGuiKey_F12, true));
	BOOST_TEST(saves == 1);
	// A key that no action is bound to fires nothing.
	BOOST_TEST(!frames.press(actions, ImGuiKey_F10, false));
	BOOST_TEST(toggles == 1);
	BOOST_TEST(saves == 1);
}

BOOST_AUTO_TEST_CASE(InGameMode_OnlyTheModeToggleAndThePlayShortcutsFire)
{
	shortcut_frames frames;
	edt::action_registry actions;
	int toggles = 0;
	int plays = 0;
	int saves = 0;
	auto toggle_reg = actions.add(make_action("editor.mode.toggle", "Game Mode", "View", [&] { ++toggles; }, "F11"));
	auto play_reg = actions.add(make_action("play.start", "Play", "Play", [&] { ++plays; }, "Ctrl+P"));
	auto save_reg = actions.add(make_action("file.save", "Save", "File", [&] { ++saves; }, "Ctrl+S"));
	const std::function<bool(std::string_view)> game = edt::is_game_mode_shortcut;

	BOOST_TEST(!frames.press(actions, ImGuiKey_S, false, true, game));
	BOOST_TEST(saves == 0);
	BOOST_TEST(frames.press(actions, ImGuiKey_F11, false, false, game));
	BOOST_TEST(toggles == 1);
	BOOST_TEST(frames.press(actions, ImGuiKey_P, false, true, game));
	BOOST_TEST(plays == 1);

	// Editor mode: every shortcut.
	BOOST_TEST(frames.press(actions, ImGuiKey_S, false, true));
	BOOST_TEST(saves == 1);
	BOOST_TEST(frames.press(actions, ImGuiKey_F11, false));
	BOOST_TEST(toggles == 2);

	for (const char* id : { "play.pause", "play.step", "play.stop" }) BOOST_TEST(edt::is_game_mode_shortcut(id));
	BOOST_TEST(!edt::is_game_mode_shortcut("play.resume"));
	BOOST_TEST(!edt::is_game_mode_shortcut("edit.undo"));
}

BOOST_AUTO_TEST_CASE(DrawingTheMainMenu_FiresNoShortcut)
{
	shortcut_frames frames;
	edt::action_registry actions;
	edt::panel_registry panels;
	int toggles = 0;
	auto reg = actions.add(make_action("t.toggle", "Toggle", "View", [&] { ++toggles; }, "F11"));

	// The pass fires once per press; the menu bar no longer does it a second time.
	ImGui::GetIO().AddKeyEvent(ImGuiKey_F11, true);
	ImGui::NewFrame();
	BOOST_TEST(edt::fire_shortcuts(actions));
	ImGui::Begin("host", nullptr, ImGuiWindowFlags_MenuBar);
	if (ImGui::BeginMenuBar()) {
		edt::draw_main_menu(actions, panels);
		ImGui::EndMenuBar();
	}
	ImGui::End();
	ImGui::EndFrame();
	BOOST_TEST(toggles == 1);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtActionRegistryTests)

BOOST_AUTO_TEST_CASE(Run_ExecutesTheRegisteredAction)
{
	edt::action_registry actions;
	int runs = 0;
	auto reg = actions.add(make_action("x.run", "Run", "File", [&] { ++runs; }));
	BOOST_CHECK(reg.active());
	BOOST_CHECK(actions.contains("x.run"));
	BOOST_CHECK(actions.activate("x.run"));
	BOOST_CHECK_EQUAL(runs, 1);
	BOOST_CHECK(!actions.activate("x.missing"));
}

BOOST_AUTO_TEST_CASE(ItemsAndMenus_FollowRegistrationOrder)
{
	edt::action_registry actions;
	auto a = actions.add(make_action("a", "First", "File", [] {}));
	auto b = actions.add(make_action("b", "Tools item", "Tools", [] {}));
	auto c = actions.add(make_action("c", "Second", "File", [] {}));
	auto d = actions.add(make_action("d", "Third", "File", [] {}));

	BOOST_TEST((actions.menus() == std::vector<std::string>{ "File", "Tools" }));
	BOOST_TEST((labels_of(actions.items("File")) == std::vector<std::string>{ "First", "Second", "Third" }));
	BOOST_TEST((labels_of(actions.items("Tools")) == std::vector<std::string>{ "Tools item" }));
	BOOST_CHECK(actions.items("Missing").empty());
}

BOOST_AUTO_TEST_CASE(DeclaredMenus_KeepTheirOrderWhateverRegistersFirst)
{
	edt::action_registry actions;
	auto file = actions.add_menu("File");
	auto view = actions.add_menu("View");
	auto help = actions.add_menu("Help");
	auto tools = actions.add(make_action("t", "Tool", "Tools", [] {}));
	auto about = actions.add(make_action("h", "About", "Help", [] {}));
	auto open = actions.add(make_action("f", "Open", "File", [] {}));

	BOOST_TEST((actions.menus() == std::vector<std::string>{ "File", "View", "Help", "Tools" }));
	help.reset();
	BOOST_TEST((actions.menus() == std::vector<std::string>{ "File", "View", "Tools", "Help" })); // "About" still names it, but no longer pins the place
	about.reset();
	BOOST_TEST((actions.menus() == std::vector<std::string>{ "File", "View", "Tools" }));
}

BOOST_AUTO_TEST_CASE(Items_CarrySeparatorsAndShortcutText)
{
	edt::action_registry actions;
	auto a = actions.add(make_action("a", "Save", "File", [] {}, "Ctrl+S"));
	auto desc = make_action("b", "Exit", "File", [] {});
	desc.separator_before = true;
	desc.shortcut_hint = "Alt+F4";
	auto b = actions.add(std::move(desc));

	const auto items = actions.items("File");
	BOOST_REQUIRE_EQUAL(items.size(), 2u);
	BOOST_CHECK_EQUAL(items[0].shortcut_text, "Ctrl+S");
	BOOST_CHECK(!items[0].separator_before);
	BOOST_CHECK_EQUAL(items[1].shortcut_text, "Alt+F4");
	BOOST_CHECK(items[1].separator_before);
}

BOOST_AUTO_TEST_CASE(ActionWithoutMenuPath_HasNoItem)
{
	edt::action_registry actions;
	int runs = 0;
	auto reg = actions.add(make_action("hidden", "Hidden", "", [&] { ++runs; }, "Ctrl+H"));
	BOOST_CHECK(actions.menus().empty());
	BOOST_CHECK(actions.dispatch(chord("Ctrl+H")));
	BOOST_CHECK_EQUAL(runs, 1);
}

BOOST_AUTO_TEST_CASE(DisabledAction_DoesNotExecute)
{
	edt::action_registry actions;
	int runs = 0;
	bool allowed = false;
	auto desc = make_action("guarded", "Guarded", "File", [&] { ++runs; }, "Ctrl+G");
	desc.enabled = [&] { return allowed; };
	auto reg = actions.add(std::move(desc));

	BOOST_CHECK(!actions.is_enabled("guarded"));
	BOOST_CHECK(!actions.items("File")[0].enabled);
	BOOST_CHECK(!actions.activate("guarded"));
	BOOST_CHECK(!actions.dispatch(chord("Ctrl+G")));
	BOOST_CHECK_EQUAL(runs, 0);

	allowed = true;
	BOOST_CHECK(actions.items("File")[0].enabled);
	BOOST_CHECK(actions.dispatch(chord("Ctrl+G")));
	BOOST_CHECK_EQUAL(runs, 1);
}

BOOST_AUTO_TEST_CASE(Dispatch_RunsTheActionBoundToTheChord)
{
	edt::action_registry actions;
	std::vector<std::string> calls;
	auto save = actions.add(make_action("save", "Save", "File", [&] { calls.push_back("save"); }, "Ctrl+S"));
	auto open = actions.add(make_action("open", "Open", "File", [&] { calls.push_back("open"); }, "Ctrl+O"));
	auto plain = actions.add(make_action("s", "Plain S", "File", [&] { calls.push_back("s"); }, "S"));

	BOOST_CHECK(actions.dispatch(chord("Ctrl+O")));
	BOOST_CHECK(actions.dispatch(chord("Ctrl+S")));
	BOOST_CHECK(actions.dispatch(chord("S")));
	BOOST_CHECK(!actions.dispatch(chord("Ctrl+Shift+S"))); // modifiers match exactly
	BOOST_CHECK(!actions.dispatch(chord("Ctrl+X")));
	BOOST_TEST((calls == std::vector<std::string>{ "open", "save", "s" }));
}

BOOST_AUTO_TEST_CASE(Dispatch_SkipsDisabledAndFallsThroughToTheNextBinding)
{
	edt::action_registry actions;
	std::vector<std::string> calls;
	auto first = make_action("first", "First", "", [&] { calls.push_back("first"); }, "Ctrl+K");
	first.enabled = [] { return false; };
	auto a = actions.add(std::move(first));
	auto b = actions.add(make_action("second", "Second", "", [&] { calls.push_back("second"); }, "Ctrl+K"));
	BOOST_CHECK(actions.dispatch(chord("Ctrl+K")));
	BOOST_TEST((calls == std::vector<std::string>{ "second" }));
}

BOOST_AUTO_TEST_CASE(SameShortcutTwice_TheEarlierActionWins)
{
	edt::action_registry actions;
	std::vector<std::string> calls;
	auto a = actions.add(make_action("a", "A", "", [&] { calls.push_back("a"); }, "Ctrl+J"));
	auto b = actions.add(make_action("b", "B", "", [&] { calls.push_back("b"); }, "Ctrl+J"));
	BOOST_CHECK(b.active());
	BOOST_CHECK(actions.dispatch(chord("Ctrl+J")));
	BOOST_TEST((calls == std::vector<std::string>{ "a" }));
}

BOOST_AUTO_TEST_CASE(ForEachShortcut_ListsEnabledBindings)
{
	edt::action_registry actions;
	bool on = false;
	auto a = actions.add(make_action("a", "A", "", [] {}, "Ctrl+A"));
	auto desc = make_action("b", "B", "", [] {}, "Ctrl+B");
	desc.enabled = [&] { return on; };
	auto b = actions.add(std::move(desc));
	auto c = actions.add(make_action("c", "C", "File", [] {}));

	std::vector<std::string> seen;
	actions.for_each_shortcut([&](const edt::shortcut& s) { seen.push_back(s.to_string()); });
	BOOST_TEST((seen == std::vector<std::string>{ "Ctrl+A" }));
	on = true;
	seen.clear();
	actions.for_each_shortcut([&](const edt::shortcut& s) { seen.push_back(s.to_string()); });
	BOOST_TEST((seen == std::vector<std::string>{ "Ctrl+A", "Ctrl+B" }));
}

BOOST_AUTO_TEST_CASE(DuplicateId_IsRefusedAndTheFirstActionStays)
{
	edt::action_registry actions;
	int first = 0, second = 0;
	auto r1 = actions.add(make_action("same", "One", "File", [&] { ++first; }));
	auto r2 = actions.add(make_action("same", "Two", "File", [&] { ++second; }));

	BOOST_CHECK(r1.active());
	BOOST_CHECK(!r2.active());
	BOOST_CHECK_EQUAL(actions.size(), 1u);
	actions.activate("same");
	BOOST_CHECK(first == 1 && second == 0);
	r2.reset();
	BOOST_CHECK(actions.contains("same"));
}

BOOST_AUTO_TEST_CASE(InvalidDescriptions_AreRefused)
{
	edt::action_registry actions;
	BOOST_CHECK(!actions.add(make_action("", "No id", "File", [] {})).active());
	BOOST_CHECK(!actions.add(make_action("no_label", "", "File", [] {})).active());
	BOOST_CHECK(!actions.add(make_action("no_run", "No run", "File", nullptr)).active());
	BOOST_CHECK(!actions.add(make_action("bad_key", "Bad", "File", [] {}, "Ctrl+")).active());
	BOOST_CHECK(!actions.add(make_action("nested", "Nested", "File/Recent", [] {})).active());
	BOOST_CHECK(!actions.add_menu("").active());
	BOOST_CHECK_EQUAL(actions.size(), 0u);
}

BOOST_AUTO_TEST_CASE(Registration_RemovesTheActionAndItsShortcut)
{
	edt::action_registry actions;
	int runs = 0;
	{
		auto reg = actions.add(make_action("temp", "Temp", "Ext", [&] { ++runs; }, "Ctrl+T"));
		BOOST_CHECK(actions.dispatch(chord("Ctrl+T")));
	}
	BOOST_CHECK(!actions.contains("temp"));
	BOOST_CHECK(!actions.dispatch(chord("Ctrl+T")));
	BOOST_CHECK(actions.menus().empty());
	BOOST_CHECK_EQUAL(runs, 1);
}

BOOST_AUTO_TEST_CASE(RegistrationOutlivingRegistry_IsHarmless)
{
	edt::registration reg;
	{
		edt::action_registry actions;
		reg = actions.add(make_action("late", "Late", "File", [] {}));
	}
	BOOST_CHECK_NO_THROW(reg.reset());
}

BOOST_AUTO_TEST_CASE(ActionMayUnregisterItselfWhileRunning)
{
	edt::action_registry actions;
	edt::registration reg;
	int runs = 0;
	reg = actions.add(make_action("once", "Once", "File", [&] {
		++runs;
		reg.reset();
	}));
	BOOST_CHECK(actions.activate("once"));
	BOOST_CHECK_EQUAL(runs, 1);
	BOOST_CHECK(!actions.contains("once"));
}

// The callback of a menu click may drop its own registration and register the same id again (a menu that rebuilds
// itself): the removal must be effective at once, not when the callback returns.
BOOST_AUTO_TEST_CASE(ActivateCallback_MayReplaceItsOwnRegistrationUnderTheSameId)
{
	edt::action_registry actions;
	edt::registration reg;
	edt::registration replacement;
	int old_runs = 0;
	int new_runs = 0;
	bool free_inside = false;
	bool replaced_inside = false;

	edt::action_desc desc = make_action("swap", "Swap", "File", [&] { ++old_runs; });
	desc.activate = [&] {
		reg.reset();
		free_inside = !actions.contains("swap") && !actions.commands().contains("swap");
		replacement = actions.add(make_action("swap", "Swap", "File", [&] { ++new_runs; }, "Ctrl+W"));
		replaced_inside = replacement.active();
	};
	reg = actions.add(std::move(desc));

	BOOST_CHECK(actions.activate("swap"));
	BOOST_CHECK(free_inside);
	BOOST_CHECK(replaced_inside);
	BOOST_CHECK_EQUAL(old_runs, 0);
	BOOST_CHECK(actions.contains("swap"));
	BOOST_CHECK_EQUAL(actions.items("File").size(), 1u);

	BOOST_CHECK(actions.activate("swap")); // the replacement has no frontend: it runs the action itself
	BOOST_CHECK_EQUAL(new_runs, 1);
	BOOST_CHECK(actions.dispatch(chord("Ctrl+W")));
	BOOST_CHECK_EQUAL(new_runs, 2);
}

BOOST_AUTO_TEST_CASE(SharedRegistry_EngineCommandsAreInvokableAndListedAsTools)
{
	cmd::command_registry shared;
	cmd::command_desc ping;
	ping.id = "engine.ping";
	ping.invoke = [](const cmd::call&) { return cmd::result::success(); };
	auto engine_registration = shared.add(std::move(ping));

	{
		edt::action_registry actions(shared);
		BOOST_CHECK(&actions.commands() == &shared);
		auto reg = actions.add(make_action("editor.thing", "Thing", "File", [] {}));
		BOOST_CHECK(shared.contains("editor.thing"));
		BOOST_CHECK(actions.contains("engine.ping"));
		BOOST_CHECK(actions.invoke("engine.ping", boost::json::object{}).ok);

		std::vector<std::string> tool_ids;
		for (const auto* tool : actions.tools()) tool_ids.push_back(tool->id);
		BOOST_TEST((tool_ids == std::vector<std::string>{ "engine.ping", "editor.thing" }));
		// No menu item or shortcut for a command the engine registered.
		BOOST_TEST((labels_of(actions.items("File")) == std::vector<std::string>{ "Thing" }));
	}
	BOOST_CHECK(!shared.contains("editor.thing")); // the editor's part left with the action registry
	BOOST_CHECK(shared.contains("engine.ping"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtBuiltinMenuTests)

BOOST_AUTO_TEST_CASE(Menus_AreFileEditViewBuildPlayHelp_EvenBeforeAnyActionIsRegistered)
{
	edt::action_registry actions;
	auto menus = edt::register_builtin_menus(actions);
	BOOST_TEST((actions.menus() == std::vector<std::string>{ "File", "Edit", "View", "Build", "Play", "Help" }));
	BOOST_TEST((labels_of(actions.items("Help")) == std::vector<std::string>{ "About" }));
	BOOST_CHECK(actions.activate("help.about")); // a placeholder, as before
}

BOOST_AUTO_TEST_CASE(FileMenu_KeepsItsOrderSeparatorsAndShortcutText)
{
	file_menu_fixture f;
	auto menus = edt::register_builtin_menus(f.actions);

	BOOST_TEST((f.actions.menus() == std::vector<std::string>{ "File", "Edit", "View", "Build", "Play", "Help" }));
	const auto items = f.actions.items("File");
	BOOST_TEST((labels_of(items) == std::vector<std::string>{
		"New Level", "Open Level...", "Save Level", "Save Level As...", "Import Model...", "New Project...", "Open Project...",
		"Exit" }));
	const std::vector<bool> separators{ false, false, false, false, true, true, false, true };
	for (std::size_t i = 0; i < items.size(); ++i) {
		BOOST_TEST_CONTEXT(items[i].label) {
			BOOST_CHECK_EQUAL(items[i].separator_before, separators[i]);
			const std::string expected = items[i].label == "Save Level" ? "Ctrl+S"
				: items[i].label == "Save Level As..." ? "Ctrl+Shift+S" : "";
			BOOST_CHECK_EQUAL(items[i].shortcut_text, expected);
			BOOST_CHECK(items[i].enabled);
		}
	}
}

BOOST_AUTO_TEST_CASE(FileMenu_CtrlSOverwritesTheSavedLevel)
{
	file_menu_fixture f;
	BOOST_CHECK(f.actions.dispatch(chord("Ctrl+S")));
	BOOST_TEST((f.calls == std::vector<std::string>{ "quick_save" }));
}

BOOST_AUTO_TEST_CASE(FileMenu_SaveLevelItemAsksForAPath)
{
	file_menu_fixture f;
	edt::dialog_auto_answer answer;
	answer.save_as_path = "levels/b.desc";
	f.dialogs.set_auto_answer(answer);

	BOOST_CHECK(f.actions.activate("file.save_as"));
	f.gui.drain();
	BOOST_TEST((f.calls == std::vector<std::string>{ "save_as:levels/b.desc" }));
}

BOOST_AUTO_TEST_CASE(FileMenu_SaveItemOverwritesATitledLevelWithoutADialog)
{
	file_menu_fixture f;
	const std::string id = file_item_with_shortcut(f.actions, "Ctrl+S");
	BOOST_REQUIRE(!id.empty());

	BOOST_CHECK(f.actions.activate(id));
	f.gui.drain();
	BOOST_TEST((f.calls == std::vector<std::string>{ "quick_save" }));
	BOOST_TEST(f.dialogs.pending() == 0u);
}

BOOST_AUTO_TEST_CASE(FileMenu_SaveItemOnAnUntitledLevelOpensSaveAs)
{
	file_menu_fixture f;
	f.path.clear();
	const std::string id = file_item_with_shortcut(f.actions, "Ctrl+S");
	BOOST_REQUIRE(!id.empty());

	BOOST_CHECK(f.actions.activate(id));
	BOOST_TEST(f.dialogs.pending() == 1u);
	BOOST_TEST(f.calls.empty());

	edt::dialog_auto_answer answer;
	answer.save_as_path = "levels/b.desc";
	f.dialogs.set_auto_answer(answer);
	f.gui.drain();
	BOOST_TEST((f.calls == std::vector<std::string>{ "save_as:levels/b.desc" }));
}

BOOST_AUTO_TEST_CASE(FileMenu_CtrlShiftSDispatchesSaveAsEvenForATitledLevel)
{
	file_menu_fixture f;
	edt::dialog_auto_answer answer;
	answer.save_as_path = "levels/b.desc";
	f.dialogs.set_auto_answer(answer);

	BOOST_CHECK_EQUAL(file_item_with_shortcut(f.actions, "Ctrl+Shift+S"), "file.save_as");
	BOOST_CHECK(f.actions.dispatch(chord("Ctrl+Shift+S")));
	f.gui.drain();
	BOOST_TEST((f.calls == std::vector<std::string>{ "save_as:levels/b.desc" }));
}

BOOST_AUTO_TEST_CASE(FileMenu_ExitWithUnsavedChangesWaitsForTheAnswer)
{
	file_menu_fixture f;
	f.dirty = true;
	edt::dialog_auto_answer answer;
	answer.unsaved = edt::unsaved_choice::cancel;
	f.dialogs.set_auto_answer(answer);

	BOOST_CHECK(f.actions.activate("file.exit"));
	f.gui.drain();
	BOOST_CHECK(f.calls.empty());

	answer.unsaved = edt::unsaved_choice::discard;
	f.dialogs.set_auto_answer(answer);
	BOOST_CHECK(f.actions.activate("file.exit"));
	f.gui.drain();
	BOOST_TEST((f.calls == std::vector<std::string>{ "exit" }));
}

BOOST_AUTO_TEST_CASE(FileMenu_DestroyedMenuTakesItsActionsAway)
{
	file_menu_fixture f;
	BOOST_CHECK(f.actions.contains("file.exit"));
	f.menu.reset();
	BOOST_CHECK_EQUAL(f.actions.size(), 0u);
	BOOST_CHECK(!f.actions.dispatch(chord("Ctrl+S")));
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
	boost::json::value json_of(std::string_view text)
	{
		return boost::json::parse(text);
	}
}

BOOST_AUTO_TEST_SUITE(EdtActionSchemaTests)

namespace {
	edt::action_desc echo_action(int& runs)
	{
		edt::action_desc desc;
		desc.id = "test.echo";
		desc.label = "Echo";
		desc.args_schema = json_of(R"({"type":"object","properties":{"text":{"type":"string"},"times":{"type":"integer","minimum":1}},
			"required":["text"],"additionalProperties":false})");
		desc.result_schema = json_of(R"({"type":"object","properties":{"echo":{"type":"string"}},"required":["echo"],"additionalProperties":false})");
		desc.execute = [&runs](const boost::json::value& args) {
			++runs;
			return edt::action_result::success(boost::json::object{ { "echo", args.as_object().at("text") } });
		};
		return desc;
	}
}

BOOST_AUTO_TEST_CASE(Mutates_DefaultsToTrueAndIsCarriedToTheCommand)
{
	edt::action_registry actions;
	int runs = 0;
	edt::action_desc unflagged = echo_action(runs);
	unflagged.id = "test.unflagged";
	edt::action_desc reading = echo_action(runs);
	reading.id = "test.reading";
	reading.mutates = false;
	auto first = actions.add(std::move(unflagged));
	auto second = actions.add(std::move(reading));
	BOOST_REQUIRE(first.active());
	BOOST_REQUIRE(second.active());

	BOOST_REQUIRE(actions.find("test.unflagged") != nullptr);
	BOOST_CHECK(actions.find("test.unflagged")->mutates);
	BOOST_REQUIRE(actions.find("test.reading") != nullptr);
	BOOST_CHECK(!actions.find("test.reading")->mutates);
}

BOOST_AUTO_TEST_CASE(Invoke_ValidatesThenRunsAndReturnsTheResult)
{
	edt::action_registry actions;
	int runs = 0;
	auto reg = actions.add(echo_action(runs));
	BOOST_REQUIRE(reg.active());

	const auto outcome = actions.invoke("test.echo", json_of(R"({"text":"hi","times":2})"));
	BOOST_CHECK(outcome.ok);
	BOOST_CHECK_EQUAL(boost::json::serialize(outcome.value), R"({"echo":"hi"})");
	BOOST_CHECK_EQUAL(runs, 1);
}

BOOST_AUTO_TEST_CASE(Invoke_RejectsInvalidArgumentsWithoutRunning)
{
	edt::action_registry actions;
	int runs = 0;
	auto reg = actions.add(echo_action(runs));

	for (const char* args : { "{}", R"({"text":3})", R"({"text":"x","times":0})", R"({"text":"x","extra":true})", "[]", "null" }) {
		const auto outcome = actions.invoke("test.echo", json_of(args));
		BOOST_TEST_CONTEXT(args) {
			BOOST_CHECK(!outcome.ok);
			BOOST_CHECK_EQUAL(outcome.code, "invalid_args");
			BOOST_CHECK(outcome.error.find("invalid arguments") != std::string::npos);
		}
	}
	BOOST_CHECK_EQUAL(runs, 0);
}

BOOST_AUTO_TEST_CASE(Invoke_FailsForMissingAndDisabledActions)
{
	edt::action_registry actions;
	int runs = 0;
	auto desc = echo_action(runs);
	desc.enabled = [] { return false; };
	auto reg = actions.add(std::move(desc));

	BOOST_CHECK_EQUAL(actions.invoke("test.missing", json_of("{}")).code, "not_found");
	const auto outcome = actions.invoke("test.echo", json_of(R"({"text":"x"})"));
	BOOST_CHECK(!outcome.ok);
	BOOST_CHECK_EQUAL(outcome.code, "disabled");
	BOOST_CHECK_EQUAL(runs, 0);
}

BOOST_AUTO_TEST_CASE(Invoke_ReportsAFailingAction)
{
	edt::action_registry actions;
	edt::action_desc desc;
	desc.id = "test.fail";
	desc.label = "Fail";
	desc.execute = [](const boost::json::value&) { return edt::action_result::failure("failed", "nope"); };
	auto reg = actions.add(std::move(desc));
	const auto outcome = actions.invoke("test.fail", json_of("{}"));
	BOOST_CHECK(!outcome.ok);
	BOOST_CHECK_EQUAL(outcome.code, "failed");
	BOOST_CHECK_EQUAL(outcome.error, "nope");
}

BOOST_AUTO_TEST_CASE(Invoke_RejectsAResultThatBreaksItsSchema)
{
	edt::action_registry actions;
	edt::action_desc desc;
	desc.id = "test.bad_result";
	desc.label = "Bad result";
	desc.result_schema = json_of(R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})");
	desc.execute = [](const boost::json::value&) { return edt::action_result::success(); };
	auto reg = actions.add(std::move(desc));
	const auto outcome = actions.invoke("test.bad_result", json_of("{}"));
	BOOST_CHECK(!outcome.ok);
	BOOST_CHECK_EQUAL(outcome.code, "result_invalid");
	BOOST_CHECK(outcome.error.find("ran but returned a malformed result") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Add_FillsDefaultSchemasAndVersion)
{
	edt::action_registry actions;
	auto reg = actions.add(make_action("plain", "Plain", "", [] {}));
	const auto* desc = actions.find("plain");
	BOOST_REQUIRE(desc != nullptr);
	BOOST_CHECK(desc->args_schema == edt::no_args_schema());
	BOOST_CHECK(desc->result_schema == edt::no_args_schema());
	BOOST_CHECK_EQUAL(desc->version, 1u);
}

BOOST_AUTO_TEST_CASE(Add_RefusesBadSchemasAndUnactivatableMenuItems)
{
	edt::action_registry actions;
	int runs = 0;
	auto bad_schema = echo_action(runs);
	bad_schema.id = "bad";
	bad_schema.args_schema = json_of(R"({"type":"nonsense"})");
	BOOST_CHECK(!actions.add(std::move(bad_schema)).active());

	auto bad_version = echo_action(runs);
	bad_version.id = "bad_version";
	bad_version.schema_version = 0;
	BOOST_CHECK(!actions.add(std::move(bad_version)).active());

	auto unsupported = echo_action(runs);
	unsupported.id = "unsupported";
	unsupported.result_schema = json_of(R"({"type":"object","pattern":"x"})");
	BOOST_CHECK(!actions.add(std::move(unsupported)).active());

	auto not_an_object = echo_action(runs);
	not_an_object.id = "not_an_object";
	not_an_object.args_schema = json_of(R"({"type":"string"})");
	BOOST_CHECK(!actions.add(std::move(not_an_object)).active());

	auto untyped = echo_action(runs);
	untyped.id = "untyped";
	untyped.args_schema = json_of(R"({"properties":{}})");
	BOOST_CHECK(!actions.add(std::move(untyped)).active());

	auto in_menu = echo_action(runs);
	in_menu.id = "in_menu";
	in_menu.menu_path = "File"; // required arguments, nothing to collect them
	BOOST_CHECK(!actions.add(std::move(in_menu)).active());

	auto with_frontend = echo_action(runs);
	with_frontend.id = "with_frontend";
	with_frontend.menu_path = "File";
	with_frontend.activate = [] {};
	BOOST_CHECK(actions.add(std::move(with_frontend)).active());
}

BOOST_AUTO_TEST_CASE(Activate_RunsTheFrontendWhichInvokesTheSameAction)
{
	edt::action_registry actions;
	int runs = 0;
	auto desc = echo_action(runs);
	desc.menu_path = "File";
	edt::action_registry* registry = &actions;
	desc.activate = [registry] { registry->invoke("test.echo", boost::json::object{ { "text", "from menu" } }); };
	auto reg = actions.add(std::move(desc));

	BOOST_CHECK(actions.activate("test.echo"));
	BOOST_CHECK_EQUAL(runs, 1);
	BOOST_CHECK(actions.invoke("test.echo", json_of(R"({"text":"from script"})")).ok);
	BOOST_CHECK_EQUAL(runs, 2);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtBuiltinActionSchemaTests)

BOOST_AUTO_TEST_CASE(EveryRegisteredActionHasValidSchemas)
{
	file_menu_fixture f;
	auto menus = edt::register_builtin_menus(f.actions);
	const auto ids = f.actions.ids();
	BOOST_CHECK_EQUAL(ids.size(), 9u);
	for (const auto& id : ids) {
		const auto* desc = f.actions.find(id);
		BOOST_REQUIRE(desc != nullptr);
		BOOST_TEST_CONTEXT(id) {
			BOOST_CHECK(!cmd::json_schema::check_schema(desc->args_schema));
			BOOST_CHECK(!cmd::json_schema::check_schema(desc->result_schema));
			BOOST_CHECK(desc->args_schema.as_object().at("type") == "object");
			BOOST_CHECK(desc->version >= 1u);
			BOOST_CHECK(static_cast<bool>(desc->invoke));
		}
	}
}

BOOST_AUTO_TEST_CASE(Tools_SkipPlaceholdersButTheMenuStillShowsThem)
{
	file_menu_fixture f;
	auto menus = edt::register_builtin_menus(f.actions);
	std::vector<std::string> tools;
	for (const auto* desc : f.actions.tools()) tools.push_back(desc->id);
	BOOST_TEST((tools == std::vector<std::string>{
		"file.new", "file.open", "file.save", "file.save_as", "file.import_model", "file.new_project", "file.open_project",
		"file.exit" }));
	BOOST_CHECK(f.actions.contains("help.about"));
	BOOST_TEST((labels_of(f.actions.items("Help")) == std::vector<std::string>{ "About" }));
}

BOOST_AUTO_TEST_CASE(EachBuiltinActionRunsHeadlessWithJsonArguments)
{
	file_menu_fixture f;
	auto menus = edt::register_builtin_menus(f.actions);

	BOOST_CHECK(f.actions.invoke("file.new", json_of(R"({})")).ok);
	BOOST_CHECK(f.actions.invoke("file.open", json_of(R"({"path":"/res/levels/a.desc"})")).ok);
	BOOST_CHECK(f.actions.invoke("file.save_as", json_of(R"({"path":"levels/b.desc"})")).ok);
	BOOST_CHECK(f.actions.invoke("file.save", json_of("{}")).ok);
	BOOST_CHECK(f.actions.invoke("file.import_model", json_of(R"({"path":"/models/m.glb"})")).ok);
	BOOST_CHECK(f.actions.invoke("file.exit", json_of("{}")).ok);
	BOOST_CHECK(f.actions.invoke("help.about", json_of("{}")).ok);
	BOOST_TEST((f.calls == std::vector<std::string>{
		"new", "load:/res/levels/a.desc", "save_as:levels/b.desc", "quick_save", "import:/models/m.glb", "exit" }));
	BOOST_CHECK(f.gui.callbacks.empty()); // no dialog was asked for
}

// While the start level of --game loads (#518) nobody may replace it: file.open and file.new answer busy, then run.
BOOST_AUTO_TEST_CASE(LevelChanges_AreBusyWhileTheStartLevelLoads)
{
	file_menu_fixture f;
	std::optional<std::string> blocker = "the start level is loading";
	edt::action_registry actions;
	edt::file_menu_hooks hooks = f.hooks;
	hooks.level_change_blocker = [&blocker] { return blocker; };
	edt::file_menu menu(f.dialogs, actions, hooks);
	menu.register_actions();

	const auto open = actions.invoke("file.open", json_of(R"({"path":"/res/levels/a.desc"})"));
	BOOST_CHECK_EQUAL(open.code, "busy");
	BOOST_CHECK_EQUAL(open.error, "the start level is loading");
	BOOST_CHECK_EQUAL(actions.invoke("file.new", json_of(R"({"discard_unsaved":true})")).code, "busy");
	BOOST_CHECK(f.calls.empty());

	blocker.reset(); // finish_startup
	BOOST_CHECK(actions.invoke("file.open", json_of(R"({"path":"/res/levels/a.desc"})")).ok);
	BOOST_CHECK(actions.invoke("file.new", json_of("{}")).ok);
	BOOST_TEST((f.calls == std::vector<std::string>{ "load:/res/levels/a.desc", "new" }));
}

// An editor built without assimp sets no import hook: the item stays in the menu, disabled, with the reason.
BOOST_AUTO_TEST_CASE(ImportModel_WithoutAnImportHookIsDisabledWithAReason)
{
	file_menu_fixture f;
	edt::action_registry actions;
	edt::file_menu_hooks hooks = f.hooks;
	hooks.import_model = nullptr;
	edt::file_menu menu(f.dialogs, actions, hooks);
	menu.register_actions();

	BOOST_CHECK(!actions.is_enabled("file.import_model"));
	BOOST_CHECK_EQUAL(actions.invoke("file.import_model", json_of(R"({"path":"/models/m.glb"})")).code, "disabled");
	bool found = false;
	for (const auto& item : actions.items("File")) {
		if (item.id != "file.import_model") continue;
		found = true;
		BOOST_CHECK(!item.enabled);
		BOOST_CHECK(!item.disabled_tooltip.empty());
	}
	BOOST_CHECK(found);
}

BOOST_AUTO_TEST_CASE(BuiltinActions_RejectBadArgumentsAndDoNotRun)
{
	file_menu_fixture f;
	const auto code = [&f](const char* id, const char* args) { return f.actions.invoke(id, json_of(args)).code; };
	BOOST_CHECK_EQUAL(code("file.open", "{}"), "invalid_args");
	BOOST_CHECK_EQUAL(code("file.open", R"({"path":4})"), "invalid_args");
	BOOST_CHECK_EQUAL(code("file.open", R"({"path":""})"), "invalid_args"); // minLength 1
	BOOST_CHECK_EQUAL(code("file.save_as", R"({"path":""})"), "invalid_args");
	BOOST_CHECK_EQUAL(code("file.import_model", R"({"path":""})"), "invalid_args");
	BOOST_CHECK_EQUAL(code("file.save_as", R"({"path":"a","x":1})"), "invalid_args");
	BOOST_CHECK_EQUAL(code("file.new", R"({"discard_unsaved":"yes"})"), "invalid_args");
	BOOST_CHECK_EQUAL(code("file.exit", R"({"other":true})"), "invalid_args");
	BOOST_CHECK_EQUAL(code("file.exit", "[]"), "invalid_args");
	BOOST_CHECK_EQUAL(code("file.exit", "null"), "invalid_args");
	BOOST_CHECK(f.calls.empty());
}

BOOST_AUTO_TEST_CASE(NewAndExit_NeedDiscardUnsavedWhenTheLevelIsDirty)
{
	file_menu_fixture f;
	f.dirty = true;
	const auto code = [&f](const char* id, const char* args) { return f.actions.invoke(id, json_of(args)).code; };
	BOOST_CHECK_EQUAL(code("file.new", "{}"), "needs_input");
	BOOST_CHECK_EQUAL(code("file.exit", R"({"discard_unsaved":false})"), "needs_input");
	BOOST_CHECK_EQUAL(code("file.open", R"({"path":"/res/a.desc"})"), "needs_input");
	BOOST_CHECK(f.calls.empty());
	BOOST_CHECK(f.dirty); // a refused call changed nothing

	BOOST_CHECK_EQUAL(code("file.new", R"({"discard_unsaved":true})"), "ok");
	BOOST_CHECK(!f.dirty); // the flag really dropped the changes
	BOOST_TEST((f.calls == std::vector<std::string>{ "new" }));
}

BOOST_AUTO_TEST_CASE(Open_WithDiscardUnsavedReplacesADirtyLevel)
{
	file_menu_fixture f;
	f.dirty = true;
	BOOST_CHECK(f.actions.invoke("file.open", json_of(R"({"path":"/res/a.desc","discard_unsaved":true})")).ok);
	BOOST_TEST((f.calls == std::vector<std::string>{ "load:/res/a.desc" }));
}

BOOST_AUTO_TEST_CASE(SaveActions_FailWithoutALevelOrAPath)
{
	file_menu_fixture f;
	f.path.clear();
	BOOST_CHECK_EQUAL(f.actions.invoke("file.save", json_of("{}")).code, "failed"); // untitled: needs file.save_as
	f.save_ok = false;
	BOOST_CHECK_EQUAL(f.actions.invoke("file.save_as", json_of(R"({"path":"levels/x.desc"})")).code, "failed");
	f.has_scene = false;
	BOOST_CHECK_EQUAL(f.actions.invoke("file.save_as", json_of(R"({"path":"levels/x.desc"})")).code, "failed");
}

BOOST_AUTO_TEST_CASE(MenuClick_AndInvokeReachTheSameCode)
{
	file_menu_fixture f;
	edt::dialog_auto_answer answer;
	answer.picked_file = std::filesystem::path("/res/levels/a.desc");
	answer.save_as_path = "levels/b.desc";
	f.dialogs.set_auto_answer(answer);

	BOOST_CHECK(f.actions.activate("file.open"));
	BOOST_CHECK(f.actions.activate("file.save_as"));
	f.gui.drain();
	const auto clicked = f.calls;
	f.calls.clear();

	BOOST_CHECK(f.actions.invoke("file.open", json_of(R"({"path":"/res/levels/a.desc"})")).ok);
	BOOST_CHECK(f.actions.invoke("file.save_as", json_of(R"({"path":"levels/b.desc"})")).ok);
	BOOST_TEST((f.calls == clicked));
	BOOST_TEST((clicked == std::vector<std::string>{ "load:/res/levels/a.desc", "save_as:levels/b.desc" }));
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

// The File menu again, with the paths of the project faked, called as a person (ui) or as an agent.
struct agent_menu
{
	explicit agent_menu(file_menu_fixture& fixture)
		: base(fixture)
	{
		hooks = fixture.hooks;
		hooks.resources_path = "/proj/res";
		hooks.load_level_tag = [this](const std::string& path) -> std::optional<std::string> {
			base.calls.push_back("load_tag:" + path);
			return std::nullopt;
		};
		hooks.inspect_level = [this](const std::string& path) {
			const auto found = files.find(path);
			return found == files.end() ? edt::level_file_state::missing : found->second;
		};
		menu = std::make_unique<edt::file_menu>(fixture.dialogs, actions, hooks);
		menu->register_actions();
	}

	cmd::result as_agent(std::string_view id, std::string_view args)
	{
		cmd::call request;
		request.args = boost::json::parse(args);
		request.from = cmd::origin::agent("session-1");
		return actions.invoke(id, request);
	}

	cmd::result as_person(std::string_view id, std::string_view args)
	{
		return actions.invoke(id, boost::json::parse(args));
	}

	file_menu_fixture& base;
	edt::action_registry actions;
	edt::file_menu_hooks hooks;
	std::map<std::string, edt::level_file_state> files;
	std::unique_ptr<edt::file_menu> menu;
};

const std::vector<std::string> bad_agent_paths{
	"../x.desc", "levels/../../x.desc", "res://../x.desc", "/etc/x.desc", "C:/x.desc", "C:\\x.desc", "\\x.desc", "levels\\x.desc",
	"memory://x.desc", "code://x.desc", "res://levels//x.desc", "levels/./x.desc", "levels/x.txt", "levels/.desc", "levels/x.desc/",
	"levels/x\ny.desc", "res://", "x",
	"levels/CON.desc", "levels/con.txt.desc", "levels/Nul.desc", "levels/aux/x.desc", "PRN/x.desc", "levels/COM1.desc", "levels/lpt9.desc",
	"levels/com0.desc", "levels/x.desc.", "levels/dir./x.desc", "levels/dir /x.desc", "levels/a<b.desc", "levels/a|b.desc",
	"levels/a?b.desc", "levels/a*b.desc", "levels/a\"b.desc", "levels/a>b.desc" };

}

BOOST_AUTO_TEST_SUITE(EdtFileMenuAgentTests)

BOOST_AUTO_TEST_CASE(SaveSaveAsAndOpen_AreSceneEditCommandsForAgents)
{
	file_menu_fixture f;
	agent_menu m(f);
	for (const char* id : { "file.save", "file.save_as", "file.open" }) {
		const cmd::command_desc* command = m.actions.commands().find(id);
		BOOST_TEST_CONTEXT(id) {
			BOOST_REQUIRE(command != nullptr);
			BOOST_CHECK((command->category == cmd::agent_category::scene_edit));
		}
	}
	// Nothing else of the menu is open to an agent (file.import_model has its own tests, #548).
	for (const char* id : { "file.new", "file.new_project", "file.open_project", "file.exit" }) {
		const cmd::command_desc* command = m.actions.commands().find(id);
		BOOST_TEST_CONTEXT(id) {
			BOOST_REQUIRE(command != nullptr);
			BOOST_CHECK((command->category == cmd::agent_category::unset));
		}
	}
}

BOOST_AUTO_TEST_CASE(AnAgent_SavesTheLevelAndSavesItAsAnotherPath)
{
	file_menu_fixture f;
	agent_menu m(f);
	BOOST_CHECK(m.as_agent("file.save", "{}").ok);
	BOOST_CHECK(m.as_agent("file.save_as", R"({"path":"levels/new.desc"})").ok);
	BOOST_CHECK(m.as_agent("file.save_as", R"({"path":"res://levels/other.desc"})").ok);
	BOOST_TEST((f.calls == std::vector<std::string>{ "quick_save", "save_as:levels/new.desc", "save_as:levels/other.desc" }));
}

BOOST_AUTO_TEST_CASE(AnAgent_OpensAnotherLevelByATagOrAProjectPath)
{
	file_menu_fixture f;
	agent_menu m(f);
	m.files["levels/a.desc"] = edt::level_file_state::file;

	const cmd::result by_path = m.as_agent("file.open", R"({"path":"levels/a.desc"})");
	BOOST_REQUIRE_MESSAGE(by_path.ok, by_path.error);
	BOOST_CHECK(by_path.value.as_object().at("level_loading").as_bool());
	BOOST_CHECK(m.as_agent("file.open", R"({"path":"res://levels/a.desc"})").ok);
	BOOST_TEST((f.calls == std::vector<std::string>{ "load_tag:levels/a.desc", "load_tag:levels/a.desc" }));

	BOOST_CHECK_EQUAL(m.as_agent("file.open", R"({"path":"levels/none.desc"})").code, "not_found");
	m.files["levels/link.desc"] = edt::level_file_state::unsafe;
	BOOST_CHECK_EQUAL(m.as_agent("file.open", R"({"path":"levels/link.desc"})").code, "invalid_args");
	BOOST_CHECK_EQUAL(f.calls.size(), 2u);
}

BOOST_AUTO_TEST_CASE(AnAgent_CannotNameAPathOutsideTheProject_ForSaveAsOrOpen)
{
	file_menu_fixture f;
	agent_menu m(f);
	for (const std::string& path : bad_agent_paths) {
		m.files[path] = edt::level_file_state::file; // even if the file state said it is fine
		const std::string args = boost::json::serialize(boost::json::object{ { "path", path } });
		BOOST_TEST_CONTEXT("path: " << path) {
			BOOST_CHECK_EQUAL(m.as_agent("file.save_as", args).code, "invalid_args");
			BOOST_CHECK_EQUAL(m.as_agent("file.open", args).code, "invalid_args");
		}
	}
	BOOST_CHECK(f.calls.empty());
}

BOOST_AUTO_TEST_CASE(AnAgent_CannotSaveOverAnotherFile_ButMaySaveOverTheCurrentLevel)
{
	file_menu_fixture f;
	agent_menu m(f);
	f.path = "levels/saved.desc";
	m.files["levels/saved.desc"] = edt::level_file_state::file;
	m.files["levels/other.desc"] = edt::level_file_state::file;
	m.files["levels/link.desc"] = edt::level_file_state::unsafe;

	const cmd::result other = m.as_agent("file.save_as", R"({"path":"levels/other.desc"})");
	BOOST_CHECK(!other.ok);
	BOOST_CHECK_EQUAL(other.code, "needs_input");
	BOOST_CHECK_EQUAL(m.as_agent("file.save_as", R"({"path":"res://levels/other.desc"})").code, "needs_input");
	BOOST_CHECK_EQUAL(m.as_agent("file.save_as", R"({"path":"levels/link.desc"})").code, "invalid_args");
	BOOST_CHECK(f.calls.empty());

	BOOST_CHECK(m.as_agent("file.save_as", R"({"path":"levels/saved.desc"})").ok);
	BOOST_CHECK(m.as_agent("file.save_as", R"({"path":"levels/fresh.desc"})").ok);
	BOOST_TEST((f.calls == std::vector<std::string>{ "save_as:levels/saved.desc", "save_as:levels/fresh.desc" }));
}

BOOST_AUTO_TEST_CASE(AnAgent_OpenOfADirtyLevelNeedsInput_AndDiscardUnsavedIsRejected)
{
	file_menu_fixture f;
	agent_menu m(f);
	m.files["levels/a.desc"] = edt::level_file_state::file;
	f.dirty = true;

	const cmd::result dirty = m.as_agent("file.open", R"({"path":"levels/a.desc"})");
	BOOST_CHECK(!dirty.ok);
	BOOST_CHECK_EQUAL(dirty.code, "needs_input");
	BOOST_CHECK(f.dirty);
	BOOST_CHECK(f.calls.empty());

	for (const char* args : { R"({"path":"levels/a.desc","discard_unsaved":true})" }) {
		BOOST_TEST_CONTEXT(args) {
			BOOST_CHECK_EQUAL(m.as_agent("file.open", args).code, "invalid_args");
			f.dirty = false;
			BOOST_CHECK_EQUAL(m.as_agent("file.open", args).code, "invalid_args"); // refused when clean too
			f.dirty = true;
		}
	}
	BOOST_CHECK(f.dirty);
	BOOST_CHECK(f.calls.empty());

	// Saved first, it opens.
	BOOST_CHECK(m.as_agent("file.save", "{}").ok);
	f.dirty = false;
	BOOST_CHECK(m.as_agent("file.open", R"({"path":"levels/a.desc","discard_unsaved":false})").ok);
	BOOST_TEST((f.calls == std::vector<std::string>{ "quick_save", "load_tag:levels/a.desc" }));
}

BOOST_AUTO_TEST_CASE(AnAgent_IsStillHeldByTheStartLevelBlocker)
{
	file_menu_fixture f;
	agent_menu m(f);
	m.files["levels/a.desc"] = edt::level_file_state::file;
	edt::action_registry actions;
	edt::file_menu_hooks hooks = m.hooks;
	hooks.level_change_blocker = [] { return std::optional<std::string>("the start level is loading"); };
	edt::file_menu menu(f.dialogs, actions, hooks);
	menu.register_actions();
	cmd::call request;
	request.args = boost::json::parse(R"({"path":"levels/a.desc"})");
	request.from = cmd::origin::agent("session-1");
	BOOST_CHECK_EQUAL(actions.invoke("file.open", request).code, "busy");
	BOOST_CHECK(f.calls.empty());
}

// While a level loads the document does not belong to its file yet: nobody saves it, a person included.
BOOST_AUTO_TEST_CASE(SaveAndSaveAs_AreBusyWhileALevelLoads)
{
	file_menu_fixture f;
	std::optional<std::string> loading = "a level is loading";
	edt::action_registry actions;
	edt::file_menu_hooks hooks = f.hooks;
	hooks.level_write_blocker = [&loading] { return loading; };
	hooks.level_change_blocker = [&loading] { return loading; };
	hooks.inspect_level = [](const std::string&) { return edt::level_file_state::missing; };
	edt::file_menu menu(f.dialogs, actions, hooks);
	menu.register_actions();
	const auto call = [&actions](cmd::origin from, const char* id, const char* args) {
		cmd::call request;
		request.args = boost::json::parse(args);
		request.from = std::move(from);
		return actions.invoke(id, request).code;
	};
	for (const cmd::origin& from : { cmd::origin::agent("s1"), cmd::origin::ui() }) {
		BOOST_TEST_CONTEXT("origin kind " << static_cast<int>(from.kind)) {
			BOOST_CHECK_EQUAL(call(from, "file.save", "{}"), "busy");
			BOOST_CHECK_EQUAL(call(from, "file.save_as", R"({"path":"levels/copy.desc"})"), "busy");
			BOOST_CHECK_EQUAL(call(from, "file.open", R"({"path":"levels/a.desc"})"), "busy");
		}
	}
	BOOST_CHECK(f.calls.empty());

	loading.reset();
	BOOST_CHECK_EQUAL(call(cmd::origin::agent("s1"), "file.save", "{}"), "ok");
	BOOST_CHECK_EQUAL(call(cmd::origin::ui(), "file.save_as", R"({"path":"levels/copy.desc"})"), "ok");
	BOOST_TEST((f.calls == std::vector<std::string>{ "quick_save", "save_as:levels/copy.desc" }));
}

BOOST_AUTO_TEST_CASE(AnAgentOpen_ALoadThatRefusesTheFileIsInvalidArgs)
{
	file_menu_fixture f;
	agent_menu m(f);
	m.files["prefabs/p.desc"] = edt::level_file_state::file;
	m.hooks.load_level_tag = [&f](const std::string& path) -> std::optional<std::string> {
		f.calls.push_back("load_tag:" + path);
		return "'" + path + "' is not a level file";
	};
	edt::action_registry actions;
	edt::file_menu menu(f.dialogs, actions, m.hooks);
	menu.register_actions();
	cmd::call request;
	request.args = boost::json::parse(R"({"path":"prefabs/p.desc"})");
	request.from = cmd::origin::agent("s1");
	const cmd::result refused = actions.invoke("file.open", request);
	BOOST_CHECK(!refused.ok);
	BOOST_CHECK_EQUAL(refused.code, "invalid_args");
	BOOST_CHECK(refused.error.find("not a level") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AnAgentOpen_NeedsTheLevelByTagHook)
{
	file_menu_fixture f;
	agent_menu m(f);
	m.files["levels/a.desc"] = edt::level_file_state::file;
	m.hooks.load_level_tag = nullptr;
	edt::action_registry actions;
	edt::file_menu menu(f.dialogs, actions, m.hooks);
	menu.register_actions();
	cmd::call request;
	request.args = boost::json::parse(R"({"path":"levels/a.desc"})");
	request.from = cmd::origin::agent("s1");
	BOOST_CHECK_EQUAL(actions.invoke("file.open", request).code, "failed");
	BOOST_CHECK(f.calls.empty());
}

// A person at the menu, a script that is not an agent: the behaviour of before.
BOOST_AUTO_TEST_CASE(APerson_KeepsEveryPathAndTheDiscardFlag)
{
	file_menu_fixture f;
	agent_menu m(f);
	f.dirty = true;
	BOOST_CHECK_EQUAL(m.as_person("file.open", R"({"path":"/res/levels/a.desc"})").code, "needs_input");
	BOOST_CHECK(m.as_person("file.open", R"({"path":"/res/levels/a.desc","discard_unsaved":true})").ok);
	BOOST_CHECK(m.as_person("file.save_as", R"({"path":"../anything.desc"})").ok);
	BOOST_CHECK(m.as_person("file.save_as", R"({"path":"levels/other.desc"})").ok);
	BOOST_TEST((f.calls == std::vector<std::string>{ "load:/res/levels/a.desc", "save_as:../anything.desc", "save_as:levels/other.desc" }));
}

BOOST_AUTO_TEST_CASE(CheckAgentLevelPath_StripsTheProtocolAndKeepsTheRest)
{
	BOOST_CHECK_EQUAL(edt::check_agent_level_path("res://levels/a.desc").path, "levels/a.desc");
	BOOST_CHECK_EQUAL(edt::check_agent_level_path("levels/sub/a.desc").path, "levels/sub/a.desc");
	BOOST_CHECK_EQUAL(edt::check_agent_level_path("a.desc").path, "a.desc");
	for (const std::string& path : bad_agent_paths) {
		BOOST_TEST_CONTEXT("path: " << path) {
			const edt::agent_level_path checked = edt::check_agent_level_path(path);
			BOOST_CHECK(!checked.ok());
			BOOST_CHECK(!checked.error.empty());
		}
	}
	BOOST_CHECK(!edt::check_agent_level_path(std::string(600, 'a') + ".desc").ok());
}

BOOST_AUTO_TEST_CASE(InspectLevelFile_SeesLinksFoldersAndMissingFiles)
{
	namespace fs = std::filesystem;
	test_util::temp_dir dir{ "level_file_state" };
	const fs::path root = dir.path() / "res";
	dir.write("res/levels/a.desc", "{}");
	dir.write("outside/target.desc", "{}");
	fs::create_directories(root / "levels" / "folder.desc");

	BOOST_CHECK((edt::inspect_level_file(root, "levels/a.desc") == edt::level_file_state::file));
	BOOST_CHECK((edt::inspect_level_file(root, "levels/none.desc") == edt::level_file_state::missing));
	BOOST_CHECK((edt::inspect_level_file(root, "nothing/deeper/none.desc") == edt::level_file_state::missing));
	BOOST_CHECK((edt::inspect_level_file(root, "levels/folder.desc") == edt::level_file_state::unsafe));
	BOOST_CHECK((edt::inspect_level_file(root, "levels/a.desc/inside.desc") == edt::level_file_state::unsafe));
	BOOST_CHECK((edt::inspect_level_file({}, "levels/a.desc") == edt::level_file_state::missing));

#ifdef _WIN32
	// A junction is a folder that leads elsewhere: another way out of the project that is not a symlink.
	{
		const fs::path junction = root / "junction";
		const std::string command = "mklink /J \"" + junction.string() + "\" \"" + (dir.path() / "outside").string() + "\" >nul 2>&1";
		// Required, not skipped: a junction the runner cannot make would leave this check vacuous.
		BOOST_REQUIRE_MESSAGE(std::system(command.c_str()) == 0 && fs::exists(junction), "mklink /J did not make a junction: " << command);
		BOOST_CHECK((edt::inspect_level_file(root, "junction/target.desc") == edt::level_file_state::unsafe));
	}
#endif
	std::error_code ec;
	fs::create_symlink(dir.path() / "outside" / "target.desc", root / "levels" / "link.desc", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}
	fs::create_directory_symlink(dir.path() / "outside", root / "linkdir", ec);
	BOOST_CHECK((edt::inspect_level_file(root, "levels/link.desc") == edt::level_file_state::unsafe));
	if (!ec) BOOST_CHECK((edt::inspect_level_file(root, "linkdir/target.desc") == edt::level_file_state::unsafe));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtActionCallerTests)

namespace {
	edt::action_desc origin_action(std::vector<std::string>& seen)
	{
		edt::action_desc desc;
		desc.id = "test.who";
		desc.label = "Who";
		desc.execute_call = [&seen](const cmd::call& request) {
			seen.push_back(request.from.kind == cmd::origin_kind::agent ? "agent:" + request.from.session : "other");
			return edt::action_result::success();
		};
		return desc;
	}
}

BOOST_AUTO_TEST_CASE(ExecuteCall_SeesWhoCalls)
{
	edt::action_registry actions;
	std::vector<std::string> seen;
	auto registration = actions.add(origin_action(seen));
	BOOST_REQUIRE(registration.active());

	BOOST_CHECK(actions.invoke("test.who", boost::json::object{}).ok);
	cmd::call request;
	request.from = cmd::origin::agent("s1");
	BOOST_CHECK(actions.invoke("test.who", request).ok);
	BOOST_TEST((seen == std::vector<std::string>{ "other", "agent:s1" }));
}

BOOST_AUTO_TEST_CASE(Execute_StillRunsForEveryCaller)
{
	edt::action_registry actions;
	int runs = 0;
	edt::action_desc desc;
	desc.id = "test.plain";
	desc.label = "Plain";
	desc.execute = [&runs](const boost::json::value&) {
		++runs;
		return edt::action_result::success();
	};
	auto registration = actions.add(std::move(desc));
	BOOST_REQUIRE(registration.active());

	BOOST_CHECK(actions.invoke("test.plain", boost::json::object{}).ok);
	cmd::call request;
	request.from = cmd::origin::agent("s1");
	BOOST_CHECK(actions.invoke("test.plain", request).ok);
	BOOST_CHECK_EQUAL(runs, 2);
}

BOOST_AUTO_TEST_CASE(AnAction_NeedsExactlyOneOfExecuteAndExecuteCall)
{
	edt::action_registry actions;
	std::vector<std::string> seen;
	edt::action_desc both = origin_action(seen);
	both.execute = [](const boost::json::value&) { return edt::action_result::success(); };
	BOOST_CHECK(!actions.add(std::move(both)).active());

	edt::action_desc neither;
	neither.id = "test.none";
	neither.label = "None";
	BOOST_CHECK(!actions.add(std::move(neither)).active());
	BOOST_CHECK(!actions.contains("test.who"));
	BOOST_CHECK(!actions.contains("test.none"));
}

BOOST_AUTO_TEST_SUITE_END()
