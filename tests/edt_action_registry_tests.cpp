#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_action_registry.h>
#include "edt_file_menu.h"
#include "edt_main_menu.h"
#include <snk/editor/edt_json_schema.h>
#include <algorithm>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
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
	if (run) desc.execute = edt::no_args(std::move(run));
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
		hooks.has_scene = [this] { return has_scene; };
		hooks.is_dirty = [this] { return dirty; };
		hooks.level_path = [this] { return path; };
		hooks.new_level = [this] { calls.push_back("new"); };
		hooks.quick_save = [this] { calls.push_back("quick_save"); };
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

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtBuiltinMenuTests)

BOOST_AUTO_TEST_CASE(Menus_AreFileViewHelp_EvenBeforeAnyActionIsRegistered)
{
	edt::action_registry actions;
	auto menus = edt::register_builtin_menus(actions);
	BOOST_TEST((actions.menus() == std::vector<std::string>{ "File", "View", "Help" }));
	BOOST_TEST((labels_of(actions.items("Help")) == std::vector<std::string>{ "About" }));
	BOOST_CHECK(actions.activate("help.about")); // a placeholder, as before
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

	BOOST_CHECK(f.actions.activate("file.save_as"));
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

BOOST_AUTO_TEST_SUITE(EdtJsonSchemaTests)

BOOST_AUTO_TEST_CASE(NoArgsSchema_AcceptsOnlyAnEmptyObject)
{
	const auto schema = edt::no_args_schema();
	BOOST_CHECK(!edt::json_schema::check_schema(schema));
	BOOST_CHECK(!edt::json_schema::validate(schema, json_of("{}")));
	BOOST_CHECK(edt::json_schema::validate(schema, json_of(R"({"x":1})")));
	BOOST_CHECK(edt::json_schema::validate(schema, json_of("[]")));
	BOOST_CHECK(edt::json_schema::validate(schema, json_of("null")));
}

BOOST_AUTO_TEST_CASE(Validate_ChecksTypeRequiredEnumAndRange)
{
	const auto schema = json_of(R"({"type":"object","properties":{
		"name":{"type":"string"},"mode":{"enum":["a","b"]},"count":{"type":"integer","minimum":1,"maximum":5},
		"flag":{"type":["boolean","null"]}},"required":["name"],"additionalProperties":false})");
	BOOST_REQUIRE(!edt::json_schema::check_schema(schema));
	BOOST_CHECK(!edt::json_schema::validate(schema, json_of(R"({"name":"x","mode":"a","count":3,"flag":null})")));
	BOOST_CHECK(edt::json_schema::validate(schema, json_of(R"({})")));                           // required
	BOOST_CHECK(edt::json_schema::validate(schema, json_of(R"({"name":1})")));                   // type
	BOOST_CHECK(edt::json_schema::validate(schema, json_of(R"({"name":"x","mode":"c"})")));      // enum
	BOOST_CHECK(edt::json_schema::validate(schema, json_of(R"({"name":"x","count":0})")));       // minimum
	BOOST_CHECK(edt::json_schema::validate(schema, json_of(R"({"name":"x","count":6})")));       // maximum
	BOOST_CHECK(edt::json_schema::validate(schema, json_of(R"({"name":"x","count":1.5})")));     // integer
	BOOST_CHECK(edt::json_schema::validate(schema, json_of(R"({"name":"x","flag":"yes"})")));    // type list
	BOOST_CHECK(edt::json_schema::validate(schema, json_of(R"({"name":"x","other":1})")));       // additionalProperties
}

BOOST_AUTO_TEST_CASE(MinLength_CountsCharacters)
{
	const auto schema = json_of(R"({"type":"string","minLength":2})");
	BOOST_REQUIRE(!edt::json_schema::check_schema(schema));
	BOOST_CHECK(edt::json_schema::validate(schema, json_of(R"("")")));
	BOOST_CHECK(edt::json_schema::validate(schema, json_of(R"("a")")));
	BOOST_CHECK(!edt::json_schema::validate(schema, json_of(R"("ab")")));
	BOOST_CHECK(!edt::json_schema::validate(schema, json_of("\"\xC3\xA9\xC3\xA9\""))); // two characters, four bytes
	BOOST_CHECK(edt::json_schema::check_schema(json_of(R"({"minLength":-1})")).has_value());
	BOOST_CHECK(edt::json_schema::check_schema(json_of(R"({"minLength":"1"})")).has_value());
}

BOOST_AUTO_TEST_CASE(CheckSchema_RefusesKeywordsItDoesNotImplement)
{
	for (const char* text : { R"({"pattern":"a"})", R"({"items":{"type":"string"}})", R"({"maxLength":3})",
		R"({"oneOf":[]})", R"({"$ref":"#/x"})", R"({"default":1})",
		R"({"type":"object","properties":{"a":{"format":"uri"}}})" }) {
		BOOST_TEST(edt::json_schema::check_schema(json_of(text)).has_value(), text);
	}
	BOOST_CHECK(!edt::json_schema::check_schema(json_of(R"({"type":"string","description":"d","title":"t"})")));
}

BOOST_AUTO_TEST_CASE(AdditionalProperties_AreAllowedUnlessForbidden)
{
	const auto schema = json_of(R"({"type":"object","properties":{"a":{"type":"string"}}})");
	BOOST_CHECK(!edt::json_schema::validate(schema, json_of(R"({"a":"x","b":2})")));
}

BOOST_AUTO_TEST_CASE(CheckSchema_RejectsBrokenSchemas)
{
	for (const char* text : { "1", R"({"type":"thing"})", R"({"type":[]})", R"({"properties":3})",
		R"({"required":["a"]})", R"({"required":[1],"properties":{"a":{}}})", R"({"additionalProperties":"no"})",
		R"({"enum":[]})", R"({"minimum":"1"})", R"({"properties":{"a":{"type":"x"}}})" }) {
		BOOST_TEST(edt::json_schema::check_schema(json_of(text)).has_value(), text);
	}
}

BOOST_AUTO_TEST_SUITE_END()

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

BOOST_AUTO_TEST_CASE(Invoke_ValidatesThenRunsAndReturnsTheResult)
{
	edt::action_registry actions;
	int runs = 0;
	auto reg = actions.add(echo_action(runs));
	BOOST_REQUIRE(reg.active());

	const auto outcome = actions.invoke("test.echo", json_of(R"({"text":"hi","times":2})"));
	BOOST_CHECK(outcome.ok);
	BOOST_CHECK_EQUAL(boost::json::serialize(outcome.result), R"({"echo":"hi"})");
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
	BOOST_CHECK_EQUAL(desc->schema_version, 1u);
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
	BOOST_CHECK_EQUAL(ids.size(), 7u);
	for (const auto& id : ids) {
		const auto* desc = f.actions.find(id);
		BOOST_REQUIRE(desc != nullptr);
		BOOST_TEST_CONTEXT(id) {
			BOOST_CHECK(!edt::json_schema::check_schema(desc->args_schema));
			BOOST_CHECK(!edt::json_schema::check_schema(desc->result_schema));
			BOOST_CHECK(desc->args_schema.as_object().at("type") == "object");
			BOOST_CHECK(desc->schema_version >= 1u);
			BOOST_CHECK(static_cast<bool>(desc->execute));
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
		"file.new", "file.open", "file.save_as", "file.save", "file.import_model", "file.exit" }));
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
