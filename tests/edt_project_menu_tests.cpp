#include <boost/test/unit_test.hpp>
#include "edt_file_menu.h"
#include <snk/project/prj_create.h>
#include <snk/project/prj_project.h>
#include "test_temp_dir.hpp"
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef SNK_EDITOR_DATA_DIR
#error "SNK_EDITOR_DATA_DIR must point at Editor/data"
#endif

// File > New Project / Open Project (EPIC-42 #158): checked here, then handed to a new editor while this one exits.

namespace {
	namespace fs = std::filesystem;
	namespace json = boost::json;

	const fs::path k_empty_template = fs::path{ SNK_EDITOR_DATA_DIR } / "templates" / "projects" / "empty";

	std::string utf8_of(const fs::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

	struct project_menu_fixture
	{
		project_menu_fixture()
		{
			// Nothing is drawn: dialogs are answered through dialog_service::answer, as a script or an agent does.
			dialogs.set_registrar([](std::string_view, std::function<bool()>) {});
			make_menu();
		}

		// The editor builds the menu once, with the recent projects it read at startup.
		void make_menu(std::vector<fs::path> recent = {})
		{
			menu.reset();
			edt::file_menu_hooks hooks;
			hooks.has_scene = [] { return true; };
			hooks.is_dirty = [this] { return dirty; };
			hooks.level_path = [] { return std::string{}; };
			hooks.new_level = [] {};
			hooks.quick_save = [] { return true; };
			hooks.save_level_as = [](const std::string&) { return true; };
			hooks.discard_changes = [this] { dirty = false; discarded = true; };
			hooks.load_level = [](const fs::path&) {};
			hooks.exit = [this] { ++exits; };
			hooks.open_project = [this](const fs::path& manifest, bool allow) -> std::optional<std::string> {
				started.push_back(manifest);
				allowed_mismatch = allow;
				return start_error;
			};
			hooks.create_project = [](const fs::path& folder, const std::string& name) {
				return prj::create_project(k_empty_template, folder, name);
			};
			hooks.projects_path = dir.path();
			hooks.recent_projects = std::move(recent);
			menu = std::make_unique<edt::file_menu>(dialogs, actions, std::move(hooks));
			menu->register_actions();
		}

		fs::path make_project(std::string_view folder)
		{
			const auto created = prj::create_project(k_empty_template, dir.path() / folder, std::string{ folder });
			BOOST_REQUIRE_MESSAGE(created.error.empty(), created.error);
			return created.manifest_file;
		}

		edt::action_result invoke(std::string_view id, std::string_view args)
		{
			return actions.invoke(id, json::parse(args));
		}

		test_util::temp_dir dir{ "edt project menu" };
		edt::dialog_service dialogs;
		edt::action_registry actions;
		std::unique_ptr<edt::file_menu> menu;
		bool dirty = false;
		bool discarded = false;
		int exits = 0;
		std::vector<fs::path> started;
		bool allowed_mismatch = false;
		std::optional<std::string> start_error;
	};

	std::string path_args(const fs::path& path, std::string_view more = {})
	{
		return std::string{ R"({"path":)" } + json::serialize(json::value(utf8_of(path))) + std::string{ more } + "}";
	}
}

BOOST_FIXTURE_TEST_SUITE(EdtProjectMenuTests, project_menu_fixture)

BOOST_AUTO_TEST_CASE(OpenProject_StartsAnEditorOnTheManifestAndExits)
{
	const fs::path manifest = make_project("game");

	// The folder is enough: the manifest in it is what the new editor gets.
	const auto result = invoke("file.open_project", path_args(dir.path() / "game"));

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_REQUIRE(started.size() == 1u);
	BOOST_TEST(fs::equivalent(started[0], manifest));
	BOOST_TEST(!allowed_mismatch);
	BOOST_TEST(exits == 1);
}

BOOST_AUTO_TEST_CASE(OpenProject_ThatDoesNotOpen_KeepsThisEditor)
{
	dir.write("broken/project.snkproj", "{ not json");

	const auto result = invoke("file.open_project", path_args(dir.path() / "broken"));

	BOOST_TEST(!result.ok);
	BOOST_TEST(started.empty());
	BOOST_TEST(exits == 0);
}

BOOST_AUTO_TEST_CASE(OpenProject_ThatCannotStart_KeepsThisEditor)
{
	make_project("game");
	start_error = "no such program";

	const auto result = invoke("file.open_project", path_args(dir.path() / "game"));

	BOOST_TEST(!result.ok);
	BOOST_TEST(result.error == "no such program");
	BOOST_TEST(exits == 0);
}

BOOST_AUTO_TEST_CASE(OpenProject_WithUnsavedChanges_NeedsTheirFate)
{
	make_project("game");
	dirty = true;

	BOOST_TEST(invoke("file.open_project", path_args(dir.path() / "game")).code == "needs_input");
	BOOST_TEST(started.empty());

	BOOST_TEST(invoke("file.open_project", path_args(dir.path() / "game", R"(,"discard_unsaved":true)")).ok);
	BOOST_TEST(discarded);
	BOOST_TEST(exits == 1);
}

BOOST_AUTO_TEST_CASE(OpenProject_PassesTheMismatchFlagOn)
{
	make_project("game");

	BOOST_TEST(invoke("file.open_project", path_args(dir.path() / "game", R"(,"allow_engine_version_mismatch":true)")).ok);
	BOOST_TEST(allowed_mismatch);
}

BOOST_AUTO_TEST_CASE(NewProject_IsMadeFromTheTemplateAndOpened)
{
	const auto result = invoke("file.new_project", path_args(dir.path() / "fresh"));

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_REQUIRE(started.size() == 1u);
	BOOST_TEST(fs::equivalent(started[0], dir.path() / "fresh" / "project.snkproj"));
	const auto opened = prj::open_project(started[0]);
	BOOST_REQUIRE(opened.context.has_value());
	BOOST_TEST(opened.context->data.name == "fresh"); // the folder's name when none is given
	BOOST_TEST(exits == 1);
}

BOOST_AUTO_TEST_CASE(NewProject_IntoATakenFolder_KeepsThisEditor)
{
	dir.write("taken/keep.txt", "keep");

	const auto result = invoke("file.new_project", path_args(dir.path() / "taken", R"(,"name":"Taken")"));

	BOOST_TEST(!result.ok);
	BOOST_TEST(started.empty());
	BOOST_TEST(exits == 0);
}

BOOST_AUTO_TEST_CASE(NewProject_FromTheMenu_AsksForTheFolder)
{
	menu->new_project();
	const auto shown = dialogs.list();
	BOOST_REQUIRE(shown.size() == 1u);
	BOOST_TEST(shown[0].kind == "path");

	BOOST_TEST(dialogs.answer(shown[0].id, json::parse(path_args(dir.path() / "from_menu"))).ok);

	BOOST_REQUIRE(started.size() == 1u);
	BOOST_TEST(fs::is_regular_file(dir.path() / "from_menu" / "project.snkproj"));
}

BOOST_AUTO_TEST_CASE(NewProject_FromTheMenu_AFailureIsShown)
{
	dir.write("taken/keep.txt", "keep");
	menu->new_project();

	BOOST_TEST(dialogs.answer(dialogs.list()[0].id, json::parse(path_args(dir.path() / "taken"))).ok);

	const auto shown = dialogs.list();
	BOOST_REQUIRE(shown.size() == 1u);
	BOOST_TEST(shown[0].kind == "message");
	BOOST_TEST(exits == 0);
}

BOOST_AUTO_TEST_CASE(RecentProjects_AreItemsOfTheFileMenuInTheirOrder)
{
	const fs::path first = make_project("first");
	const fs::path second = make_project("second");
	make_menu({ first, second });

	const auto items = actions.items("File");
	std::vector<const edt::menu_item_info*> recent;
	for (const auto& item : items) {
		if (item.id.starts_with("file.recent_project.")) recent.push_back(&item);
	}
	BOOST_REQUIRE(recent.size() == 2u);
	BOOST_TEST(recent[0]->id == "file.recent_project.0");
	BOOST_TEST(recent[0]->label.starts_with("1. first"));
	BOOST_TEST(recent[0]->separator_before);
	BOOST_TEST(recent[1]->id == "file.recent_project.1");
	BOOST_TEST(recent[1]->label.starts_with("2. second"));
	BOOST_TEST(!recent[1]->separator_before);
	BOOST_TEST(items.back().id == "file.exit");
	// file.open_project is the one agents call.
	for (const auto* tool : actions.tools()) BOOST_TEST(!tool->id.starts_with("file.recent_project."));
}

BOOST_AUTO_TEST_CASE(RecentProject_StartsAnEditorOnItAndExits)
{
	const fs::path manifest = make_project("game");
	make_menu({ manifest });

	const auto result = invoke("file.recent_project.0", "{}");

	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_REQUIRE(started.size() == 1u);
	BOOST_TEST(fs::equivalent(started[0], manifest));
	BOOST_TEST(!allowed_mismatch);
	BOOST_TEST(exits == 1);
}

BOOST_AUTO_TEST_CASE(RecentProject_WithUnsavedChanges_AsksFirst)
{
	make_menu({ make_project("game") });
	dirty = true;

	BOOST_TEST(invoke("file.recent_project.0", "{}").code == "needs_input");
	BOOST_TEST(started.empty());
	BOOST_TEST(invoke("file.recent_project.0", R"({"discard_unsaved":true})").ok);
	BOOST_TEST(started.size() == 1u);
}

BOOST_AUTO_TEST_CASE(RecentProject_ThatIsGone_IsDisabled)
{
	make_menu({ dir.path() / "gone" / "project.snkproj" });

	BOOST_TEST(!actions.is_enabled("file.recent_project.0"));
	BOOST_TEST(!invoke("file.recent_project.0", "{}").ok);
	BOOST_TEST(started.empty());
	BOOST_TEST(exits == 0);
}

BOOST_AUTO_TEST_SUITE_END()
