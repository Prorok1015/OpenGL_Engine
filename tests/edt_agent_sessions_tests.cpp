#include <boost/test/unit_test.hpp>
#include "edt_agent_sessions.h"
#include "edt_scene_commands.h"
#include "edt_scene_document.h"
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_selection_service.h>
#include <snk/scene/level/scn_world_desc.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// agent.session.* and the "changed by agent" marks (A5, #291 part 2): a session's edits are one undo record per
// step, its rollback stops before a person's edit of the same node, and what an agent changed stays marked until a save
// or an accept.

namespace {

namespace json = boost::json;
using prefab_node = scn::prefab_desc::prefab_node;

struct sessions_fixture
{
	sessions_fixture()
	{
		scn::world_desc world;
		world.get_root().node_id = 1;
		for (const auto& [id, name] : std::vector<std::pair<std::uint64_t, std::string>>{ { 10, "a" }, { 20, "b" }, { 30, "c" }, { 40, "d" } }) {
			prefab_node node;
			node.node_id = id;
			node.name = name;
			world.get_root().children.push_back(node);
		}
		scene.world_descs.push_back(std::move(world));
		scene.world_names.push_back("w0");
		scene.active_world_idx = 0;
		scene.reset_history();
		scene.mark_clean();
		edits = edt::register_scene_commands(commands, { documents, selection, services, {} });
		session_commands = sessions.register_commands(commands);
	}

	cmd::result call(const std::string& id, const std::string& args = "{}", cmd::origin from = cmd::origin::agent("s1"))
	{
		return commands.invoke(id, json::parse(args), from);
	}

	static std::string ref(const char* id)
	{
		return std::string(R"({"context":"w0","path":[")") + id + R"("]})";
	}

	cmd::result rename(const char* id, const std::string& name, cmd::origin from = cmd::origin::agent("s1"))
	{
		return call("scene.node.rename", R"({"ref":)" + ref(id) + R"(,"name":")" + name + R"("})", std::move(from));
	}

	// A person's edit, as the hierarchy makes it.
	void person_renames(std::size_t index, const std::string& name)
	{
		scene.world_descs[0].get_root().children[index].name = name;
		scene.commit("Rename node");
	}

	const std::string& name(std::size_t index) { return scene.world_descs[0].get_root().children[index].name; }

	bool marked(std::uint64_t id) const { return scene.agent_changes().contains({ 0, id }); }

	edt::editor_services services;
	edt::scene_document scene;
	edt::editor_events events;
	edt::document_manager documents{ scene, services, events };
	edt::selection_service selection{ events };
	cmd::command_registry commands;
	edt::agent_sessions sessions{ documents };
	std::vector<edt::registration> edits;
	std::vector<edt::registration> session_commands;
};

constexpr const char* A = "000000000000000a";
constexpr const char* B = "0000000000000014";
constexpr const char* C = "000000000000001e";

}

BOOST_FIXTURE_TEST_SUITE(edt_agent_sessions_tests, sessions_fixture)

BOOST_AUTO_TEST_CASE(the_session_commands_are_scene_edits)
{
	for (const char* id : { "agent.session.begin", "agent.session.checkpoint", "agent.session.end", "agent.session.rollback", "agent.session.get" }) {
		BOOST_TEST_CONTEXT(id) {
			const auto* command = commands.find(id);
			BOOST_REQUIRE(command != nullptr);
			BOOST_TEST((command->category == cmd::agent_category::scene_edit));
		}
	}
	BOOST_TEST(!commands.find("agent.session.get")->mutates);
}

BOOST_AUTO_TEST_CASE(a_session_is_one_record_per_step)
{
	BOOST_REQUIRE(call("agent.session.begin", R"({"label":"Tidy up"})").ok);
	BOOST_REQUIRE(rename(A, "a2").ok);
	BOOST_REQUIRE(rename(B, "b2").ok);
	BOOST_TEST(scene.history().size() == 1u);
	BOOST_TEST(scene.history().records().back().label == "Tidy up");

	BOOST_REQUIRE(call("agent.session.checkpoint", R"({"label":"Step 2"})").ok);
	BOOST_REQUIRE(rename(C, "c2").ok);
	BOOST_TEST(scene.history().size() == 2u);

	const auto state = call("agent.session.get");
	BOOST_REQUIRE(state.ok);
	BOOST_TEST(state.value.at("open").as_bool());
	BOOST_TEST(state.value.at("steps").as_uint64() == 2u);
	BOOST_TEST(state.value.at("records").as_uint64() == 2u);

	const auto undone = call("agent.session.rollback");
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST(undone.value.at("rolled_back").as_uint64() == 3u);
	BOOST_TEST(!undone.value.at("open").as_bool());
	BOOST_TEST(name(0) == "a");
	BOOST_TEST(name(1) == "b");
	BOOST_TEST(name(2) == "c");
	BOOST_TEST(!call("agent.session.get").value.at("open").as_bool());
}

BOOST_AUTO_TEST_CASE(a_rollback_stops_at_the_persons_edit_and_stays_open)
{
	BOOST_REQUIRE(call("agent.session.begin", R"({"label":"Tidy up"})").ok);
	BOOST_REQUIRE(rename(B, "agent b").ok);
	person_renames(1, "person b");
	BOOST_REQUIRE(rename(C, "agent c").ok);

	const auto undone = call("agent.session.rollback");
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST(undone.value.at("rolled_back").as_uint64() == 1u);
	BOOST_TEST(undone.value.at("remaining").as_uint64() == 1u);
	BOOST_TEST(undone.value.at("open").as_bool());
	const auto& at = undone.value.at("stopped_at").as_object();
	BOOST_TEST(at.at("key").as_string() == "node:0:0000000000000014");
	BOOST_TEST(at.at("label").as_string() == "Rename node");
	BOOST_TEST(json::serialize(at.at("ref")) == ref(B));
	BOOST_TEST(name(1) == "person b");
	BOOST_TEST(name(2) == "c");

	BOOST_TEST(call("agent.session.get").value.at("open").as_bool());
	BOOST_REQUIRE(call("agent.session.end").ok);
	BOOST_TEST(!call("agent.session.get").value.at("open").as_bool());
}

BOOST_AUTO_TEST_CASE(after_a_conflict_in_an_earlier_step_the_next_edits_get_a_step_of_their_own)
{
	BOOST_REQUIRE(call("agent.session.begin", R"({"label":"Tidy up"})").ok);
	BOOST_REQUIRE(rename(B, "agent b").ok);
	BOOST_REQUIRE(call("agent.session.checkpoint").ok);
	BOOST_REQUIRE(rename(C, "agent c").ok);
	person_renames(1, "person b");

	const auto undone = call("agent.session.rollback");
	BOOST_REQUIRE(undone.ok);
	BOOST_TEST(undone.value.at("open").as_bool());
	BOOST_TEST(name(2) == "c");

	const auto records = scene.history().size();
	BOOST_REQUIRE(rename(A, "agent a").ok);
	BOOST_TEST(scene.history().size() == records + 1);
	BOOST_TEST(call("agent.session.get").value.at("open").as_bool());
}

BOOST_AUTO_TEST_CASE(a_level_load_ends_the_session)
{
	BOOST_REQUIRE(call("agent.session.begin", R"({"label":"Tidy up"})").ok);
	BOOST_REQUIRE(rename(A, "a2").ok);
	scene.reset_history();

	BOOST_TEST(!call("agent.session.get").value.at("open").as_bool());
	BOOST_TEST(call("agent.session.checkpoint").code == cmd::codes::failed);
	BOOST_TEST(call("agent.session.rollback").code == cmd::codes::failed);
}

BOOST_AUTO_TEST_CASE(a_closed_mcp_session_ends_its_session_without_rolling_back)
{
	BOOST_REQUIRE(call("agent.session.begin", R"({"label":"Tidy up"})").ok);
	BOOST_REQUIRE(rename(A, "a2").ok);

	sessions.session_closed("s1");

	BOOST_TEST(name(0) == "a2");
	BOOST_TEST(!call("agent.session.get").value.at("open").as_bool());
	// The group is closed: the next edit is a record of its own.
	BOOST_REQUIRE(rename(B, "b2").ok);
	BOOST_TEST(scene.history().size() == 2u);
}

BOOST_AUTO_TEST_CASE(sessions_of_two_agents_are_their_own)
{
	BOOST_REQUIRE(call("agent.session.begin", R"({"label":"One"})", cmd::origin::agent("s1")).ok);
	BOOST_REQUIRE(call("agent.session.begin", R"({"label":"Two"})", cmd::origin::agent("s2")).ok);
	BOOST_REQUIRE(rename(A, "a1", cmd::origin::agent("s1")).ok);
	BOOST_REQUIRE(rename(C, "c2", cmd::origin::agent("s2")).ok);

	BOOST_REQUIRE(call("agent.session.rollback", "{}", cmd::origin::agent("s2")).ok);
	BOOST_TEST(name(0) == "a1");
	BOOST_TEST(name(2) == "c");
	BOOST_TEST(call("agent.session.get", "{}", cmd::origin::agent("s1")).value.at("open").as_bool());
}

BOOST_AUTO_TEST_CASE(what_an_agent_changed_is_marked_until_a_save)
{
	BOOST_REQUIRE(rename(B, "agent b").ok);
	BOOST_TEST(marked(20));
	BOOST_TEST(scene.has_agent_changes());

	person_renames(0, "person a");
	BOOST_TEST(!marked(10));

	BOOST_REQUIRE(scene.undo()); // the person's
	BOOST_REQUIRE(scene.undo()); // the agent's
	BOOST_TEST(!marked(20));
	BOOST_REQUIRE(scene.redo());
	BOOST_TEST(marked(20));

	scene.mark_clean();
	BOOST_TEST(!scene.has_agent_changes());
}

BOOST_AUTO_TEST_CASE(an_accept_clears_the_marks_and_later_edits_of_the_session_are_marked_again)
{
	BOOST_REQUIRE(call("agent.session.begin", R"({"label":"Tidy up"})").ok);
	BOOST_REQUIRE(rename(A, "a2").ok);
	scene.accept_agent_changes();
	BOOST_TEST(!scene.has_agent_changes());

	// Folded into the same record as the accepted edit, and still new.
	BOOST_REQUIRE(rename(B, "b2").ok);
	BOOST_TEST(scene.history().size() == 1u);
	BOOST_TEST(marked(20));
	BOOST_TEST(!marked(10));
}

BOOST_AUTO_TEST_CASE(a_node_that_only_moved_up_is_not_marked)
{
	BOOST_REQUIRE(call("scene.node.delete", R"({"ref":)" + ref(C) + "}").ok);
	BOOST_TEST(marked(30)); // what the agent deleted
	BOOST_TEST(!marked(40)); // d only took c's place

	BOOST_REQUIRE(call("scene.node.reparent", R"({"ref":)" + ref(A) + R"(,"parent":)" + ref(B) + "}").ok);
	BOOST_TEST(marked(10)); // moved under another node: the agent's doing
}

BOOST_AUTO_TEST_SUITE_END()
