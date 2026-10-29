#include <boost/test/unit_test.hpp>
#include "edt_agent_policy.h"
#include "edt_edit_menu.h"
#include "edt_mcp_bridge_host.h"
#include "edt_main_menu.h"
#include "edt_scene_document.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
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
		dialogs.set_registrar([](std::string_view, std::function<bool()>) {});
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
	edt::dialog_service dialogs;
	bool gesture = false;
	std::vector<edt::registration> menus = edt::register_builtin_menus(actions);
	std::vector<edt::registration> registered = edt::register_edit_actions(actions, documents, dialogs, [this] { return gesture; });
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
	BOOST_TEST(undone.value.as_object().at("origin").as_string() == "ui");
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

	// The text of the menu says so; the result carries the label as recorded and the origin apart.
	const auto undone = actions.invoke("edit.undo", boost::json::object{});
	BOOST_REQUIRE(undone.ok);
	BOOST_TEST(undone.value.as_object().at("label").as_string() == "Rename node");
	BOOST_TEST(undone.value.as_object().at("origin").as_string() == "agent");
}

BOOST_AUTO_TEST_CASE(nothing_is_undone_behind_a_dialog)
{
	rename("b");
	const auto prompt = dialogs.confirm_unsaved([](edt::unsaved_choice) {});
	BOOST_REQUIRE(dialogs.pending() == 1u);

	BOOST_TEST(!actions.is_enabled("edit.undo"));
	BOOST_TEST(!actions.dispatch(*edt::shortcut::parse("Ctrl+Z")));
	BOOST_TEST(name() == "b");

	BOOST_REQUIRE(dialogs.answer(prompt, boost::json::parse(R"({"choice":"cancel"})")).ok);
	BOOST_REQUIRE(dialogs.pending() == 0u);
	BOOST_TEST(actions.dispatch(*edt::shortcut::parse("Ctrl+Z")));
	BOOST_TEST(name() == "a");
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
		BOOST_CHECK_MESSAGE(!edt::agent_may_use(*command), id);
	}
	BOOST_TEST(!actions.find("edit.redo_alt")->expose_as_tool);
}

BOOST_AUTO_TEST_CASE(accepting_the_agents_changes_clears_their_marks)
{
	BOOST_TEST(!actions.is_enabled("edit.accept_agent_changes"));
	{
		edt::edit_origin_scope scope(scene, cmd::origin::agent("s1"));
		rename("b");
	}
	BOOST_REQUIRE(scene.has_agent_changes());
	BOOST_TEST(actions.is_enabled("edit.accept_agent_changes"));

	const auto accepted = actions.invoke("edit.accept_agent_changes", boost::json::object{});
	BOOST_REQUIRE(accepted.ok);
	BOOST_TEST(accepted.value.as_object().at("accepted").as_bool());
	BOOST_TEST(!scene.has_agent_changes());
	BOOST_TEST(!actions.is_enabled("edit.accept_agent_changes"));
	// Accepting is not undoing: the edit stays, and so does its record.
	BOOST_TEST(name() == "b");
	BOOST_TEST(scene.history().can_undo());
	// What a person has looked at is theirs to say: no policy file hands it to an agent.
	const auto* accept = actions.find("edit.accept_agent_changes");
	BOOST_TEST((accept->category == cmd::agent_category::not_for_agents));
	edt::policy_layer allowed;
	allowed.commands["edit.accept_agent_changes"] = cmd::policy_mode::allow;
	BOOST_TEST((edt::effective_mode(*accept, allowed, {}).mode == cmd::policy_mode::deny));
}

BOOST_AUTO_TEST_SUITE_END()
