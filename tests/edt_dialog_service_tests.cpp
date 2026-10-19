#include <boost/test/unit_test.hpp>
#include "edt_file_menu.h"
#include <snk/editor/edt_dialog_service.h>
#include <algorithm>
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
};

struct menu_fixture : dialog_fixture
{
	menu_fixture()
	{
		hooks.has_scene = [this] { return has_scene; };
		hooks.is_dirty = [this] { return dirty; };
		hooks.level_path = [this] { return path; };
		hooks.new_level = [this] { calls.push_back("new"); };
		hooks.quick_save = [this] { calls.push_back("quick_save"); };
		hooks.save_level_as = [this](const std::string& p) {
			calls.push_back("save_as:" + p);
			return save_ok;
		};
		hooks.discard_changes = [this] { calls.push_back("discard"); };
		hooks.load_level = [this](const std::filesystem::path& p) { calls.push_back("load:" + p.generic_string()); };
		hooks.import_model = [this](const std::filesystem::path& p) { calls.push_back("import:" + p.generic_string()); };
		hooks.exit = [this] { calls.push_back("exit"); };
		menu = std::make_unique<edt::file_menu>(service, hooks);
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
	std::string path;
	std::vector<std::string> calls;
	edt::file_menu_hooks hooks;
	std::unique_ptr<edt::file_menu> menu;
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
