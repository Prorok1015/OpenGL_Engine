#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_action_registry.h>
#include "edt_file_menu.h"
#include "edt_main_menu.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// action_registry, the File menu registered through it and the built-in menu order, all without ImGui.
// Not covered headless: the ImGui drawing of the menu bar and the key lookup of the shortcuts.

namespace {

edt::action_desc make_action(std::string id, std::string label, std::string menu, std::function<void()> run,
	std::string shortcut_text = {})
{
	edt::action_desc desc;
	desc.id = std::move(id);
	desc.label = std::move(label);
	desc.menu_path = std::move(menu);
	desc.shortcut = std::move(shortcut_text);
	desc.execute = std::move(run);
	return desc;
}

std::vector<std::string> labels_of(const std::vector<edt::menu_item_info>& items)
{
	std::vector<std::string> out;
	for (const auto& item : items) out.push_back(item.label);
	return out;
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
		hooks.has_scene = [] { return true; };
		hooks.is_dirty = [this] { return dirty; };
		hooks.level_path = [this] { return path; };
		hooks.new_level = [this] { calls.push_back("new"); };
		hooks.quick_save = [this] { calls.push_back("quick_save"); };
		hooks.save_level_as = [this](const std::string& p) { calls.push_back("save_as:" + p); return true; };
		hooks.discard_changes = [] {};
		hooks.load_level = [](const std::filesystem::path&) {};
		hooks.import_model = [](const std::filesystem::path&) {};
		hooks.exit = [this] { calls.push_back("exit"); };
		menu = std::make_unique<edt::file_menu>(dialogs, hooks);
		menu->register_actions(actions);
	}

	fake_gui gui;
	edt::dialog_service dialogs;
	edt::action_registry actions;
	edt::file_menu_hooks hooks;
	std::unique_ptr<edt::file_menu> menu;
	bool dirty = false;
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

BOOST_AUTO_TEST_SUITE(EdtActionRegistryTests)

BOOST_AUTO_TEST_CASE(Run_ExecutesTheRegisteredAction)
{
	edt::action_registry actions;
	int runs = 0;
	auto reg = actions.add(make_action("x.run", "Run", "File", [&] { ++runs; }));
	BOOST_CHECK(reg.active());
	BOOST_CHECK(actions.contains("x.run"));
	BOOST_CHECK(actions.run("x.run"));
	BOOST_CHECK_EQUAL(runs, 1);
	BOOST_CHECK(!actions.run("x.missing"));
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
	BOOST_CHECK(!actions.run("guarded"));
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
	actions.run("same");
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
	BOOST_CHECK(actions.run("once"));
	BOOST_CHECK_EQUAL(runs, 1);
	BOOST_CHECK(!actions.contains("once"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtBuiltinMenuTests)

BOOST_AUTO_TEST_CASE(Menus_AreFileViewHelp_EvenBeforeAnyActionIsRegistered)
{
	edt::action_registry actions;
	auto menus = edt::register_builtin_menus(actions);
	BOOST_TEST((actions.menus() == std::vector<std::string>{ "File", "View", "Help" }));
	BOOST_TEST((labels_of(actions.items("Help")) == std::vector<std::string>{ "About" }));
	BOOST_CHECK(actions.run("help.about")); // a placeholder, as before
}

BOOST_AUTO_TEST_CASE(FileMenu_KeepsItsOrderSeparatorsAndShortcutText)
{
	file_menu_fixture f;
	auto menus = edt::register_builtin_menus(f.actions);

	BOOST_TEST((f.actions.menus() == std::vector<std::string>{ "File", "View", "Help" }));
	const auto items = f.actions.items("File");
	BOOST_TEST((labels_of(items) == std::vector<std::string>{
		"New Level", "Open Level...", "Save Level", "Import Model...", "Exit" }));
	const std::vector<bool> separators{ false, false, false, true, true };
	for (std::size_t i = 0; i < items.size(); ++i) {
		BOOST_TEST_CONTEXT(items[i].label) {
			BOOST_CHECK_EQUAL(items[i].separator_before, separators[i]);
			BOOST_CHECK_EQUAL(items[i].shortcut_text, items[i].label == "Save Level" ? "Ctrl+S" : "");
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

	BOOST_CHECK(f.actions.run("file.save_level"));
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

	BOOST_CHECK(f.actions.run("file.exit"));
	f.gui.drain();
	BOOST_CHECK(f.calls.empty());

	answer.unsaved = edt::unsaved_choice::discard;
	f.dialogs.set_auto_answer(answer);
	BOOST_CHECK(f.actions.run("file.exit"));
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
