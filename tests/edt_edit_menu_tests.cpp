#include <boost/test/unit_test.hpp>
#include "edt_edit_menu.h"
#include "edt_main_menu.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/scene/level/scn_world_desc.h>
#include <boost/json.hpp>
#include <string>
#include <vector>

// Edit > Undo / Redo (#145): the actions over the active document's history, without ImGui.

namespace {

struct edit_menu_fixture
{
	edit_menu_fixture()
	{
		scn::world_desc world;
		world.get_root().node_id = 1;
		scn::prefab_desc::prefab_node node;
		node.node_id = 10;
		node.name = "a";
		world.get_root().children.push_back(node);
		scene.world_descs.push_back(std::move(world));
		scene.reset_history();
		scene.mark_clean();
	}

	std::string& name() { return scene.world_descs[0].get_root().children[0].name; }

	void rename(const std::string& to)
	{
		name() = to;
		scene.commit("Rename node");
	}

	std::string label(const std::string& id)
	{
		for (const auto& item : actions.items("Edit")) {
			if (item.id == id) return item.label;
		}
		return {};
	}

	edt::scene_document scene;
	edt::editor_services services;
	edt::editor_events events;
	edt::document_manager documents{ scene, services, events };
	edt::action_registry actions;
	bool gesture = false;
	std::vector<edt::registration> menus = edt::register_builtin_menus(actions);
	std::vector<edt::registration> registered = edt::register_edit_actions(actions, documents, [this] { return gesture; });
};

}

BOOST_FIXTURE_TEST_SUITE(edt_edit_menu_tests, edit_menu_fixture)

BOOST_AUTO_TEST_CASE(the_items_name_the_edit_they_apply)
{
	BOOST_TEST(label("edit.undo") == "Undo");
	BOOST_TEST(label("edit.redo") == "Redo");
	BOOST_TEST(!actions.is_enabled("edit.undo"));
	BOOST_TEST(!actions.is_enabled("edit.redo"));

	rename("b");
	BOOST_TEST(label("edit.undo") == "Undo Rename node");
	BOOST_TEST(actions.is_enabled("edit.undo"));

	BOOST_REQUIRE(actions.invoke("edit.undo", boost::json::object{}).ok);
	BOOST_TEST(label("edit.undo") == "Undo");
	BOOST_TEST(label("edit.redo") == "Redo Rename node");
}

BOOST_AUTO_TEST_CASE(undo_and_redo_go_through_the_active_document)
{
	rename("b");

	const auto undone = actions.invoke("edit.undo", boost::json::object{});
	BOOST_REQUIRE(undone.ok);
	BOOST_TEST(undone.value.as_object().at("applied").as_bool());
	BOOST_TEST(undone.value.as_object().at("label").as_string() == "Rename node");
	BOOST_TEST(name() == "a");
	BOOST_TEST(!scene.is_dirty());

	BOOST_REQUIRE(actions.invoke("edit.redo", boost::json::object{}).ok);
	BOOST_TEST(name() == "b");
	BOOST_TEST(scene.is_dirty());
}

BOOST_AUTO_TEST_CASE(ctrl_shift_z_redoes_too)
{
	rename("b");
	BOOST_REQUIRE(actions.invoke("edit.undo", boost::json::object{}).ok);

	BOOST_TEST(actions.dispatch(*edt::shortcut::parse("Ctrl+Shift+Z")));
	BOOST_TEST(name() == "b");
	// No menu item of its own.
	BOOST_TEST(label("edit.redo_alt").empty());
}

BOOST_AUTO_TEST_CASE(an_agents_edit_says_so)
{
	{
		edt::edit_origin_scope scope(scene, cmd::origin::agent("s1"));
		rename("b");
	}
	BOOST_TEST(label("edit.undo") == "Undo Rename node (agent)");
}

BOOST_AUTO_TEST_CASE(nothing_is_undone_during_a_gesture)
{
	rename("b");
	gesture = true;

	BOOST_TEST(!actions.is_enabled("edit.undo"));
	BOOST_TEST(!actions.dispatch(*edt::shortcut::parse("Ctrl+Z")));
	BOOST_TEST(name() == "b");

	gesture = false;
	BOOST_TEST(actions.dispatch(*edt::shortcut::parse("Ctrl+Z")));
	BOOST_TEST(name() == "a");
}

BOOST_AUTO_TEST_CASE(a_text_field_keeps_ctrl_z_but_not_ctrl_s)
{
	rename("b");
	bool saved = false;
	auto save = actions.add({ .id = "file.save", .label = "Save Level", .menu_path = "File", .shortcut = "Ctrl+S",
		.execute = edt::no_args([&saved] { saved = true; }) });

	BOOST_TEST(!actions.dispatch(*edt::shortcut::parse("Ctrl+Z"), true));
	BOOST_TEST(name() == "b");
	BOOST_TEST(actions.dispatch(*edt::shortcut::parse("Ctrl+S"), true));
	BOOST_TEST(saved);
}

BOOST_AUTO_TEST_CASE(the_edit_actions_are_not_offered_to_an_agent)
{
	for (const char* id : { "edit.undo", "edit.redo", "edit.redo_alt" }) {
		const auto* command = actions.find(id);
		BOOST_REQUIRE_MESSAGE(command, id);
		BOOST_CHECK_MESSAGE(command->mutates, id);
	}
	BOOST_TEST(!actions.find("edit.redo_alt")->expose_as_tool);
}

BOOST_AUTO_TEST_SUITE_END()
