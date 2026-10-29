#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_document_history.h>
#include "edt_scene_document.h"
#include "edt_scene_history.h"
#include <snk/command/cmd_types.h>
#include <snk/scene/level/scn_world_desc.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The undo history (#141-#144): document_history over fake changes, then scene_document diffing its worlds.

namespace {

// Writes "<name>+" on redo and "<name>-" on undo.
class fake_change final : public edt::history_change
{
public:
	fake_change(std::vector<std::string>& journal, std::string name, std::vector<std::string> keys = {})
		: m_journal(journal), m_name(std::move(name)), m_keys(std::move(keys)) {}

	void undo() override { m_journal.push_back(m_name + "-"); }
	void redo() override { m_journal.push_back(m_name + "+"); }
	const std::vector<std::string>& keys() const override { return m_keys; }

private:
	std::vector<std::string>& m_journal;
	std::string m_name;
	std::vector<std::string> m_keys;
};

struct history_fixture
{
	std::unique_ptr<edt::history_change> change(std::string name, std::vector<std::string> keys = {})
	{
		return std::make_unique<fake_change>(journal, std::move(name), std::move(keys));
	}

	std::vector<std::string> journal;
	edt::document_history history;
};

using prefab_node = scn::prefab_desc::prefab_node;

prefab_node make_node(std::uint64_t id, std::string name)
{
	prefab_node node;
	node.node_id = id;
	node.name = std::move(name);
	return node;
}

bool same_tree(const prefab_node& a, const prefab_node& b)
{
	if (!edt::same_shallow(a, b) || a.children.size() != b.children.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.children.size(); ++i) {
		if (!same_tree(a.children[i], b.children[i])) {
			return false;
		}
	}
	return true;
}

prefab_node* find(prefab_node& node, std::uint64_t id)
{
	if (node.node_id == id) {
		return &node;
	}
	for (auto& child : node.children) {
		if (auto* found = find(child, id)) {
			return found;
		}
	}
	return nullptr;
}

// root(1) { a(10) { a1(11) a2(12) } b(20) c(30) d(40) e(50) }
struct scene_fixture
{
	scene_fixture()
	{
		scn::world_desc world;
		world.get_root().node_id = 1;
		auto a = make_node(10, "a");
		a.children.push_back(make_node(11, "a1"));
		a.children.push_back(make_node(12, "a2"));
		world.get_root().children.push_back(std::move(a));
		const std::vector<std::pair<std::uint64_t, std::string>> rest{ { 20, "b" }, { 30, "c" }, { 40, "d" }, { 50, "e" } };
		for (const auto& [id, name] : rest) {
			world.get_root().children.push_back(make_node(id, name));
		}
		scene.world_descs.push_back(std::move(world));
		scene.reset_history();
		scene.mark_clean();
	}

	prefab_node& root() { return scene.world_descs[0].get_root(); }
	prefab_node& node(std::uint64_t id)
	{
		auto* found = find(root(), id);
		BOOST_REQUIRE(found);
		return *found;
	}

	// The edit, then undo back to the start and redo to the edit, comparing the whole tree each time.
	void check_round_trip(const std::string& label, const std::function<void()>& edit)
	{
		const prefab_node before = root();
		edit();
		scene.commit(label);
		BOOST_REQUIRE(scene.history().can_undo());
		const prefab_node after = root();

		BOOST_REQUIRE(scene.undo());
		BOOST_CHECK_MESSAGE(same_tree(root(), before), label << ": undo");
		BOOST_REQUIRE(scene.redo());
		BOOST_CHECK_MESSAGE(same_tree(root(), after), label << ": redo");
		BOOST_REQUIRE(scene.undo());
		BOOST_CHECK_MESSAGE(same_tree(root(), before), label << ": undo again");
		// The index followed the worlds: nothing left to record.
		scene.commit("nothing");
		BOOST_CHECK_MESSAGE(scene.last_commit_stats().changes == 0u, label << ": index");
	}

	edt::scene_document scene;
};

}

BOOST_FIXTURE_TEST_SUITE(edt_document_history_tests, history_fixture)

BOOST_AUTO_TEST_CASE(undo_and_redo_walk_the_records_in_order)
{
	history.record("one", cmd::origin::ui(), change("1"));
	history.record("two", cmd::origin::ui(), change("2"));
	BOOST_TEST(history.size() == 2u);
	BOOST_REQUIRE(history.next_undo());
	BOOST_TEST(history.next_undo()->label == "two");

	BOOST_TEST(history.undo());
	BOOST_TEST(history.undo());
	BOOST_TEST(!history.undo());
	BOOST_TEST(history.redo());
	BOOST_TEST(history.next_redo()->label == "two");

	BOOST_TEST((journal == std::vector<std::string>{ "2-", "1-", "1+" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(nothing_changed_is_not_recorded)
{
	history.record("none", cmd::origin::ui(), nullptr);
	BOOST_TEST(history.size() == 0u);
	BOOST_TEST(!history.can_undo());
}

BOOST_AUTO_TEST_CASE(a_new_record_drops_what_was_undone)
{
	history.record("one", cmd::origin::ui(), change("1"));
	history.record("two", cmd::origin::ui(), change("2"));
	history.undo();
	history.record("three", cmd::origin::ui(), change("3"));

	BOOST_TEST(history.size() == 2u);
	BOOST_TEST(!history.can_redo());
	BOOST_TEST(history.records().back().label == "three");
}

BOOST_AUTO_TEST_CASE(over_the_limit_the_oldest_records_go)
{
	history.set_limit(3);
	for (int i = 0; i < 5; ++i) {
		history.record("edit " + std::to_string(i), cmd::origin::ui(), change(std::to_string(i)));
	}
	BOOST_TEST(history.size() == 3u);
	BOOST_TEST(history.records().front().label == "edit 2");
}

BOOST_AUTO_TEST_CASE(undo_back_to_the_save_point_is_clean_again)
{
	BOOST_TEST(history.is_at_save_point());
	history.record("one", cmd::origin::ui(), change("1"));
	history.mark_saved();
	history.record("two", cmd::origin::ui(), change("2"));
	BOOST_TEST(!history.is_at_save_point());

	history.undo();
	BOOST_TEST(history.is_at_save_point());
	history.undo();
	BOOST_TEST(!history.is_at_save_point());
	history.redo();
	BOOST_TEST(history.is_at_save_point());
}

BOOST_AUTO_TEST_CASE(a_saved_state_that_was_undone_and_replaced_is_gone)
{
	history.record("one", cmd::origin::ui(), change("1"));
	history.mark_saved();
	history.undo();
	history.record("other", cmd::origin::ui(), change("2"));
	history.undo();

	BOOST_TEST(!history.is_at_save_point());
	BOOST_TEST(history.can_redo());
	history.redo();
	BOOST_TEST(!history.is_at_save_point());
}

BOOST_AUTO_TEST_CASE(a_group_folds_its_edits_until_someone_else_edits)
{
	const auto agent = cmd::origin::agent("s1");
	const auto group = history.begin_group("Agent session", agent);
	BOOST_TEST(history.open_group(agent) == group);

	history.record("a", agent, change("a"));
	history.record("b", agent, change("b"));
	BOOST_TEST(history.size() == 1u);
	BOOST_TEST(history.records().back().label == "Agent session");

	history.record("human", cmd::origin::ui(), change("h"));
	history.record("c", agent, change("c"));
	BOOST_TEST(history.size() == 3u);
	BOOST_TEST(history.records().back().group == group);

	history.undo();
	history.undo();
	history.undo();
	BOOST_TEST((journal == std::vector<std::string>{ "c-", "h-", "b-", "a-" }), boost::test_tools::per_element());

	history.end_group(group);
	BOOST_TEST(history.open_group(agent) == 0u);
}

BOOST_AUTO_TEST_CASE(a_rollback_undoes_the_group_and_keeps_the_rest)
{
	const auto agent = cmd::origin::agent("s1");
	const auto group = history.begin_group("Agent session", agent);
	history.record("a", agent, change("a", { "x" }));
	history.record("human", cmd::origin::ui(), change("h", { "y" }));
	history.record("b", agent, change("b", { "z" }));

	const auto result = history.rollback_group(group);

	BOOST_TEST(result.rolled_back == 2u);
	BOOST_TEST(result.remaining == 0u);
	BOOST_TEST(!result.conflict_key.has_value());
	BOOST_TEST((journal == std::vector<std::string>{ "b-", "a-" }), boost::test_tools::per_element());
	BOOST_REQUIRE(history.size() == 1u);
	BOOST_TEST(history.records().front().label == "human");
	BOOST_TEST(history.open_group(agent) == 0u);
	BOOST_TEST(!history.is_at_save_point());
}

BOOST_AUTO_TEST_CASE(a_rollback_stops_before_an_edit_someone_changed_after)
{
	const auto agent = cmd::origin::agent("s1");
	const auto group = history.begin_group("Agent session", agent);
	history.record("a", agent, change("a", { "x" }));
	history.record("b", agent, change("b", { "y" }));
	history.record("human", cmd::origin::ui(), change("h", { "x" }));
	history.record("c", agent, change("c", { "z" }));

	const auto result = history.rollback_group(group);

	// c and b go; a is under the human's edit of x.
	BOOST_TEST(result.rolled_back == 2u);
	BOOST_TEST(result.remaining == 1u);
	BOOST_REQUIRE(result.conflict_key.has_value());
	BOOST_TEST(*result.conflict_key == "x");
	BOOST_TEST(result.conflict_label == "human");
	BOOST_TEST((journal == std::vector<std::string>{ "c-", "b-" }), boost::test_tools::per_element());
	BOOST_TEST(history.size() == 2u);
	BOOST_TEST(history.open_group(agent) == group);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(edt_scene_history_tests, scene_fixture)

BOOST_AUTO_TEST_CASE(every_kind_of_edit_undoes_and_redoes)
{
	check_round_trip("create", [&] { root().children.push_back(make_node(60, "created")); });
	check_round_trip("delete", [&] { root().children.erase(root().children.begin() + 1); });
	check_round_trip("delete a subtree", [&] { root().children.erase(root().children.begin()); });
	check_round_trip("duplicate", [&] {
		auto copy = node(10);
		copy.node_id = 70;
		copy.children[0].node_id = 71;
		copy.children[1].node_id = 72;
		root().children.insert(root().children.begin() + 1, std::move(copy));
	});
	check_round_trip("reparent", [&] {
		auto moved = node(20);
		root().children.erase(root().children.begin() + 1);
		node(10).children.insert(node(10).children.begin() + 1, std::move(moved));
	});
	check_round_trip("reparent a child up", [&] {
		auto lifted = node(11);
		node(10).children.erase(node(10).children.begin());
		root().children.push_back(std::move(lifted));
	});
	check_round_trip("reorder", [&] { std::swap(root().children[1], root().children[3]); });
	check_round_trip("rename", [&] { node(30).name = "renamed"; });
	check_round_trip("transform", [&] {
		node(40).position = { 1.0f, 2.0f, 3.0f };
		node(40).has_transform = true;
	});
	check_round_trip("add a component", [&] {
		scn::prefab_desc::prefab_comp_node component;
		component.type_name = "stat_desc";
		component.overrides["hp"] = 5;
		node(50).components["stat"] = component;
	});
	check_round_trip("override", [&] {
		scn::override_record record;
		record.target = { 7 };
		record.component = "stat";
		record.path = "/hp";
		record.value = 3;
		node(12).overrides.push_back(record);
	});
}

BOOST_AUTO_TEST_CASE(deleted_siblings_come_back_in_their_order)
{
	const prefab_node before = root();
	root().children.erase(root().children.begin() + 1, root().children.begin() + 4);
	scene.commit("delete three");
	BOOST_REQUIRE(root().children.size() == 2u);

	BOOST_REQUIRE(scene.undo());
	BOOST_TEST(same_tree(root(), before));
}

BOOST_AUTO_TEST_CASE(a_moved_subtree_is_undone_with_its_children)
{
	const prefab_node before = root();
	auto a = node(10);
	root().children.erase(root().children.begin());
	node(50).children.push_back(std::move(a));
	scene.commit("move a under e");
	auto b = node(20);
	root().children.erase(root().children.begin());
	node(11).children.push_back(std::move(b));
	scene.commit("move b under a1");

	const prefab_node after = root();

	BOOST_REQUIRE(scene.undo());
	BOOST_REQUIRE(scene.undo());
	BOOST_TEST(same_tree(root(), before));
	BOOST_REQUIRE(scene.redo());
	BOOST_REQUIRE(scene.redo());
	BOOST_TEST(same_tree(root(), after));
	// Undo places by depth: the depths below the moved nodes followed them.
	BOOST_REQUIRE(scene.undo());
	BOOST_REQUIRE(scene.undo());
	BOOST_TEST(same_tree(root(), before));
	scene.commit("nothing");
	BOOST_TEST(scene.last_commit_stats().changes == 0u);
}

BOOST_AUTO_TEST_CASE(undo_to_the_save_point_makes_the_scene_clean)
{
	BOOST_TEST(!scene.is_dirty());
	node(20).name = "b2";
	scene.commit("rename");
	BOOST_TEST(scene.is_dirty());

	scene.undo();
	BOOST_TEST(!scene.is_dirty());
	scene.redo();
	BOOST_TEST(scene.is_dirty());
	scene.mark_clean();
	BOOST_TEST(!scene.is_dirty());
	scene.undo();
	BOOST_TEST(scene.is_dirty());
}

BOOST_AUTO_TEST_CASE(an_edit_that_changed_nothing_is_not_recorded)
{
	scene.commit("nothing");
	BOOST_TEST(!scene.is_dirty());
	BOOST_TEST(scene.history().size() == 0u);
	BOOST_TEST(scene.last_commit_stats().changes == 0u);
}

BOOST_AUTO_TEST_CASE(a_new_world_is_a_boundary)
{
	node(20).name = "b2";
	scene.commit("rename");
	scene.world_descs.emplace_back();
	scene.commit("Create world", edt::commit_mode::state_only);

	BOOST_TEST(scene.is_dirty());
	BOOST_TEST(!scene.history().can_undo());
	// The new world is compared from now on.
	scene.world_descs[1].get_root().children.push_back(make_node(5, "in the new world"));
	scene.commit("create");
	BOOST_TEST(scene.history().can_undo());
}

BOOST_AUTO_TEST_CASE(a_reset_keeps_the_scene_dirty_but_forgets_the_edits)
{
	node(20).name = "b2";
	scene.commit("rename");
	scene.reset_history();

	BOOST_TEST(scene.is_dirty());
	BOOST_TEST(!scene.history().can_undo());
	scene.mark_clean();
	BOOST_TEST(!scene.is_dirty());
}

BOOST_AUTO_TEST_CASE(an_id_twice_is_not_recorded_but_marks_the_scene_dirty)
{
	root().children.push_back(make_node(20, "twin"));
	scene.commit("twin");

	BOOST_TEST(scene.is_dirty());
	BOOST_TEST(!scene.history().can_undo());
}

BOOST_AUTO_TEST_CASE(a_rebase_takes_the_worlds_without_an_edit)
{
	node(30).position = { 4.0f, 0.0f, 0.0f };
	scene.rebase_history();
	scene.commit("nothing");

	BOOST_TEST(!scene.history().can_undo());
	BOOST_TEST(!scene.is_dirty());
}

BOOST_AUTO_TEST_CASE(the_agents_edits_roll_back_up_to_the_humans)
{
	const auto agent = cmd::origin::agent("s1");
	const auto group = scene.history().begin_group("Agent session", agent);
	{
		edt::edit_origin_scope scope(scene, agent);
		node(20).name = "agent b";
		scene.commit("rename b");
		node(30).name = "agent c";
		scene.commit("rename c");
	}
	BOOST_TEST((scene.edit_origin() == cmd::origin::ui()));
	node(20).name = "human b";
	scene.commit("rename b again");
	{
		edt::edit_origin_scope scope(scene, agent);
		root().children.push_back(make_node(60, "agent made"));
		scene.commit("create");
	}

	int applied = 0;
	scene.set_before_history_apply([&applied] { ++applied; });
	const auto result = scene.rollback_group(group);

	BOOST_TEST(applied == 1);
	BOOST_TEST(result.rolled_back == 2u); // the create and the rename of c
	BOOST_TEST(result.remaining == 1u);   // the rename of b, which the human renamed after
	BOOST_REQUIRE(result.conflict_key.has_value());
	BOOST_TEST(*result.conflict_key == "node:0:0000000000000014");
	BOOST_TEST(result.conflict_label == "rename b again");
	BOOST_TEST(node(20).name == "human b");
	BOOST_TEST(node(30).name == "c");
	BOOST_TEST(find(root(), 60) == nullptr);
}

BOOST_AUTO_TEST_CASE(a_commit_compares_every_node_and_copies_only_the_changed)
{
	// The root and 2000 nodes: 40 under the root with 49 children each.
	scn::world_desc world;
	world.get_root().node_id = 1;
	std::uint64_t next = 2;
	for (int i = 0; i < 40; ++i) {
		auto parent = make_node(next++, "parent");
		for (int j = 0; j < 49; ++j) {
			parent.children.push_back(make_node(next++, "child"));
		}
		world.get_root().children.push_back(std::move(parent));
	}
	constexpr std::size_t k_nodes = 1 + 40 * 50;
	BOOST_REQUIRE(next - 1 == k_nodes);
	scene.world_descs = { std::move(world) };
	scene.reset_history();
	scene.mark_clean();

	root().children[7].children[3].name = "edited";
	scene.commit("rename");
	BOOST_TEST(scene.last_commit_stats().nodes_compared == k_nodes);
	BOOST_TEST(scene.last_commit_stats().snapshots_copied == 1u);
	BOOST_TEST(scene.last_commit_stats().changes == 1u);

	// The last parent and its 49 children.
	root().children.pop_back();
	scene.commit("delete a subtree");
	BOOST_TEST(scene.last_commit_stats().nodes_compared == k_nodes - 50);
	BOOST_TEST(scene.last_commit_stats().snapshots_copied == 0u); // the removed are moved out of the index
	BOOST_TEST(scene.last_commit_stats().changes == 50u);
	BOOST_REQUIRE(scene.history().next_undo());
	BOOST_TEST(scene.history().next_undo()->changes.front()->keys().size() == 50u);

	BOOST_REQUIRE(scene.undo());
	BOOST_TEST(root().children.size() == 40u);
	BOOST_TEST(root().children.back().children.size() == 49u);
	scene.commit("nothing");
	BOOST_TEST(scene.last_commit_stats().changes == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
