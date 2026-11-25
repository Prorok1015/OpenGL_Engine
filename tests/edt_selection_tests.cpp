#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_inspector_panel.h"
#include "edt_scene_editor.h"
#include "edt_scene_hierarchy_panel.h"
#include "edt_selection_wiring.h"
#include "edt_viewport_pick_apply.h"
#include "edt_scene_document.h"
#include "edt_world_controller.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_selection_service.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <string>
#include <vector>

// selection_service and the routing that replaced the hierarchy -> inspector/viewport callbacks.
// Not covered headless: the hierarchy's own click callback (needs an ImGui frame) and the inspector's
// state (no accessors). The viewport is a std::function here; the real panel needs a gui_system.

namespace {

struct selection_fixture : test_util::scene_env
{
	selection_fixture()
	{
		state.world_descs.emplace_back();
		state.world_names.push_back("w");
		state.active_world_idx = 0;
		state.bind_events(&events);
		hierarchy.set_world_desc(&state.world_descs[0]);
		scn::node_index::get(reg); // before the first node_id_component
		service.set_registry_provider([this](const std::string& world) -> entt::registry* {
			return world == "w" ? &reg : nullptr;
		});
		connections = edt::wire_selection(events, service, state, editor, world_ctrl, hierarchy, inspector,
			[this](ecs::entity ent) {
				viewport.push_back(ent);
				order.push_back("viewport");
			});
	}

	entt::entity spawn(std::vector<uint64_t> path)
	{
		const auto ent = reg.create();
		reg.emplace<scn::node_id_component>(ent, scn::node_id_component{ std::move(path) });
		return ent;
	}

	edt::selection at(std::vector<uint64_t> path)
	{
		edt::selection value;
		value.world = "w";
		value.path = std::move(path);
		return value;
	}

	edt::editor_events events;
	edt::selection_service service{ events };
	edt::editor_services services;
	edt::scene_document state;
	edt::scene_editor editor{ state, services };
	edt::world_controller world_ctrl{ state, services };
	edt::scene_hierarchy_panel hierarchy;
	edt::inspector_panel inspector;
	entt::registry reg;
	std::vector<ecs::entity> viewport;
	std::vector<std::string> order;
	std::vector<edt::connection> connections;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtSelectionTests, selection_fixture)

BOOST_AUTO_TEST_CASE(Service_SelectAndClearEmitWithThePayload)
{
	std::vector<edt::selection> seen;
	auto c = events.selection_changed.subscribe([&seen](const edt::selection& s) { seen.push_back(s); });

	auto value = at({ 4, 5 });
	value.target = { 5 };
	value.readonly = true;
	service.select(value);
	BOOST_REQUIRE(seen.size() == 1u);
	BOOST_TEST(seen[0].world == "w");
	BOOST_TEST((seen[0].path == std::vector<uint64_t>{ 4, 5 }));
	BOOST_TEST((seen[0].target == std::vector<uint64_t>{ 5 }));
	BOOST_TEST(seen[0].readonly);
	BOOST_TEST(service.has_selection());

	service.clear();
	BOOST_REQUIRE(seen.size() == 2u);
	BOOST_TEST(seen[1].empty());
	BOOST_TEST(!service.has_selection());
}

BOOST_AUTO_TEST_CASE(Service_ResetsOnActiveWorldChange)
{
	service.select(at({ 1 }));
	std::vector<bool> empty_when_seen;
	auto c = events.selection_changed.subscribe([&](const edt::selection& s) { empty_when_seen.push_back(s.empty()); });

	events.active_world_changed.emit(0);

	BOOST_TEST(!service.has_selection());
	BOOST_TEST((empty_when_seen == std::vector<bool>{ true }));
}

BOOST_AUTO_TEST_CASE(Service_FindsTheEntityByItsPath)
{
	const auto child = spawn({ 7, 8 });
	spawn({ 7 });

	service.select(at({ 7, 8 }));
	BOOST_CHECK(service.entity() == child);

	service.select(at({ 9 }));
	BOOST_CHECK(service.entity() == entt::null);

	auto other_world = at({ 7, 8 });
	other_world.world = "elsewhere";
	service.select(other_world);
	BOOST_CHECK(service.entity() == entt::null);

	service.select(at({ 7, 8 }));
	reg.destroy(child); // a push rebuilt the world: the path no longer leads anywhere
	BOOST_CHECK(service.entity() == entt::null);

	service.clear();
	BOOST_CHECK(service.entity() == entt::null);
}

BOOST_AUTO_TEST_CASE(Service_WithoutARegistryProviderHasNoEntity)
{
	edt::editor_events other_events;
	edt::selection_service bare(other_events);
	bare.select(at({ 1 }));
	BOOST_CHECK(bare.entity() == entt::null);
}

BOOST_AUTO_TEST_CASE(Edit_ClearsTheSelectionBeforeTheDescChanges)
{
	scn::prefab_desc::prefab_node node;
	node.name = "n";
	node.node_id = 11;
	state.world_descs[0].get_root().children.push_back(node);
	hierarchy.set_selected_node(&state.world_descs[0].get_root().children[0]);
	const auto ent = spawn({ 11 });
	service.select(at({ 11 }));
	BOOST_REQUIRE(hierarchy.get_selected_node());
	viewport.clear();

	editor.create_entity("Empty"); // on_before_modify; no level is loaded, so the push itself bails out

	BOOST_TEST(!service.has_selection());
	BOOST_TEST(hierarchy.get_selected_node() == nullptr);
	BOOST_REQUIRE(viewport.size() == 1u);
	BOOST_CHECK(viewport[0] == entt::null);
	BOOST_CHECK(reg.valid(ent));
}

BOOST_AUTO_TEST_CASE(Undo_ClearsTheSelectionBeforeTheDescChanges)
{
	state.reset_history();
	scn::prefab_desc::prefab_node node;
	node.name = "n";
	node.node_id = 11;
	state.world_descs[0].get_root().children.push_back(node);
	state.commit("Create entity");
	BOOST_REQUIRE(state.history().can_undo());
	hierarchy.set_selected_node(&state.world_descs[0].get_root().children[0]);
	service.select(at({ 11 }));
	BOOST_REQUIRE(hierarchy.get_selected_node());
	int cleared = 0;
	auto counting = events.selection_changed.subscribe([&cleared](const edt::selection& sel) {
		if (sel.empty()) ++cleared;
	});

	BOOST_REQUIRE(state.undo()); // the node the hierarchy points at goes

	BOOST_TEST(cleared == 1);
	BOOST_TEST(!service.has_selection());
	BOOST_TEST(hierarchy.get_selected_node() == nullptr);
	BOOST_TEST(state.world_descs[0].get_root().children.empty());
}

BOOST_AUTO_TEST_CASE(Push_FindsTheRespawnedEntityAfterTheListenersOfDocumentChanged)
{
	auto old_ent = spawn({ 3 });
	service.select(at({ 3 }));
	viewport.clear();
	order.clear();
	reg.destroy(old_ent);
	const auto new_ent = spawn({ 3 }); // the world was rebuilt by the push

	std::vector<uint64_t> generations;
	auto later = events.document_changed.subscribe([&](edt::document_id, uint64_t generation) {
		generations.push_back(generation);
		order.push_back("later");
	});
	state.touch_descs();
	state.notify_changed();

	BOOST_REQUIRE(viewport.size() == 1u);
	BOOST_CHECK(viewport[0] == new_ent);
	BOOST_TEST((order == std::vector<std::string>{ "viewport", "later" }));
	BOOST_TEST((generations == std::vector<uint64_t>{ state.desc_generation }));
	BOOST_TEST(service.has_selection()); // a push alone does not drop the selection
}

BOOST_AUTO_TEST_CASE(Push_WithoutASelectionClearsTheViewportEntity)
{
	state.notify_changed();
	BOOST_REQUIRE(viewport.size() == 1u);
	BOOST_CHECK(viewport[0] == entt::null);
}

BOOST_AUTO_TEST_CASE(WorldSwitch_ClearsTheSelectionOnceAndBeforeLaterListeners)
{
	scn::prefab_desc::prefab_node node;
	node.node_id = 2;
	state.world_descs[0].get_root().children.push_back(node);
	hierarchy.set_selected_node(&state.world_descs[0].get_root().children[0]);
	spawn({ 2 });
	service.select(at({ 2 }));
	viewport.clear();
	order.clear();

	bool selection_was_empty = false;
	auto later = events.active_world_changed.subscribe([&](edt::document_id) {
		selection_was_empty = !service.has_selection();
		order.push_back("later");
	});
	world_ctrl.on_world_switched(0);

	BOOST_TEST(selection_was_empty);
	BOOST_TEST(hierarchy.get_selected_node() == nullptr);
	BOOST_TEST((order == std::vector<std::string>{ "viewport", "later" }));
	BOOST_REQUIRE(viewport.size() == 1u);
	BOOST_CHECK(viewport[0] == entt::null);
}

BOOST_AUTO_TEST_CASE(Service_ReentrantClearDoesNotChangeThePayloadLaterSlotsRead)
{
	bool cleared = false;
	auto first = events.selection_changed.subscribe([&](const edt::selection&) {
		if (!cleared) {
			cleared = true;
			service.clear();
		}
	});
	std::vector<std::vector<uint64_t>> seen;
	auto second = events.selection_changed.subscribe([&](const edt::selection& s) { seen.push_back(s.path); });

	service.select(at({ 7, 8 }));

	// The nested clear reaches the second slot first; the select that started it still announces its own value.
	BOOST_REQUIRE(seen.size() == 2u);
	BOOST_TEST(seen[0].empty());
	BOOST_TEST((seen[1] == std::vector<uint64_t>{ 7, 8 }));
	BOOST_TEST(!service.has_selection());
}

BOOST_AUTO_TEST_CASE(Connections_StopRoutingWhenDropped)
{
	connections.clear();
	service.select(at({ 1 }));
	state.notify_changed();
	BOOST_TEST(viewport.empty());
}

BOOST_AUTO_TEST_CASE(SelectPaths_ReplaceAddRemove_FollowTheViewportBoxRules)
{
	const auto add_node = [this](std::vector<scn::prefab_desc::prefab_node>& into, std::uint64_t id) -> scn::prefab_desc::prefab_node& {
		scn::prefab_desc::prefab_node node;
		node.node_id = id;
		node.name = "n" + std::to_string(id);
		into.push_back(node);
		return into.back();
	};
	auto& children = state.world_descs[0].get_root().children;
	children.reserve(4);
	add_node(children, 1);
	add_node(children, 2);
	add_node(children, 3);
	add_node(children[1].children, 20);
	using paths = std::vector<std::vector<uint64_t>>;
	const auto ids = [this] { return hierarchy.selected_node_ids(); };

	hierarchy.select_paths(paths{ { 1 } }, edt::pick_op::replace);
	BOOST_TEST((ids() == std::vector<uint64_t>{ 1 }));
	BOOST_TEST(service.current().path == std::vector<uint64_t>{ 1 });

	hierarchy.select_paths(paths{ { 2 }, { 3 } }, edt::pick_op::add);
	BOOST_TEST((ids() == std::vector<uint64_t>{ 1, 2, 3 }));
	BOOST_TEST(hierarchy.get_selected_node()->node_id == 3u);

	hierarchy.select_paths(paths{ { 2 } }, edt::pick_op::remove);
	BOOST_TEST((ids() == std::vector<uint64_t>{ 1, 3 }));

	int announced = 0;
	auto count = events.selection_changed.subscribe([&announced](const edt::selection&) { ++announced; });
	hierarchy.select_paths(paths{ { 2 } }, edt::pick_op::remove);
	hierarchy.select_paths(paths{}, edt::pick_op::add);
	BOOST_TEST(announced == 0);

	hierarchy.select_paths(paths{ { 3 } }, edt::pick_op::remove);
	BOOST_TEST((ids() == std::vector<uint64_t>{ 1 }));
	hierarchy.select_paths(paths{ { 1 } }, edt::pick_op::remove);
	BOOST_TEST(ids().empty());
	BOOST_TEST(service.current().empty());

	hierarchy.select_paths(paths{ { 1 }, { 2 } }, edt::pick_op::replace);
	BOOST_TEST((ids() == std::vector<uint64_t>{ 1, 2 }));
	hierarchy.select_paths(paths{}, edt::pick_op::replace);
	BOOST_TEST(ids().empty());
	BOOST_TEST(service.current().empty());
}

BOOST_AUTO_TEST_CASE(SelectPaths_OnlyAnInstanceOwnerTakesTheTailOfAPath_APlainNodeWithAnUnknownChildSkipsIt)
{
	auto& children = state.world_descs[0].get_root().children;
	children.reserve(3);
	scn::prefab_desc::prefab_node plain;
	plain.node_id = 5;
	plain.name = "plain";
	children.push_back(plain);
	scn::prefab_desc::prefab_node instance;
	instance.node_id = 6;
	instance.name = "instance";
	scn::prefab_desc::prefab_comp_node reference;
	reference.type_name = "prefab_desc";
	reference.parent_desc = rs->require<desc::desc_base>(res::tag{ "memory://t/gone_parent.prefab.desc" });
	instance.components["prefab"] = reference;
	children.push_back(instance);
	using paths = std::vector<std::vector<uint64_t>>;

	hierarchy.select_paths(paths{ { 5, 77, 78 }, { 99 }, { 6, 77, 78 } }, edt::pick_op::replace);
	BOOST_TEST((hierarchy.selected_node_ids() == std::vector<uint64_t>{ 6 }));
	hierarchy.select_paths(paths{ { 5, 77 } }, edt::pick_op::replace);
	BOOST_TEST(hierarchy.selected_node_ids().empty());
}

BOOST_AUTO_TEST_CASE(SelectPaths_TheSameSetWithTheSamePrimary_AnnouncesNothing)
{
	auto& children = state.world_descs[0].get_root().children;
	children.reserve(3);
	for (const std::uint64_t id : { 1, 2 }) {
		scn::prefab_desc::prefab_node node;
		node.node_id = id;
		children.push_back(node);
	}
	using paths = std::vector<std::vector<uint64_t>>;
	hierarchy.select_paths(paths{ { 1 }, { 2 } }, edt::pick_op::replace);
	int announced = 0;
	auto count = events.selection_changed.subscribe([&announced](const edt::selection&) { ++announced; });
	hierarchy.select_paths(paths{ { 1 }, { 2 } }, edt::pick_op::replace);
	BOOST_TEST(announced == 0);
	hierarchy.select_paths(paths{ { 2 }, { 1 } }, edt::pick_op::replace);
	BOOST_TEST(announced == 1);
}

BOOST_AUTO_TEST_CASE(ApplyViewportPick_BoxSingleClickAndEmptySpace_TakeTheirOwnBranches)
{
	auto& children = state.world_descs[0].get_root().children;
	children.reserve(4);
	for (const std::uint64_t id : { 1, 2, 3 }) {
		scn::prefab_desc::prefab_node node;
		node.node_id = id;
		children.push_back(node);
	}
	const auto ids = [this] { return hierarchy.selected_node_ids(); };
	using paths = std::vector<std::vector<uint64_t>>;

	edt::apply_viewport_pick(hierarchy, service, { paths{ { 1 }, { 2 } }, edt::pick_op::replace, true });
	BOOST_TEST((ids() == std::vector<uint64_t>{ 1, 2 }));

	edt::apply_viewport_pick(hierarchy, service, { paths{ { 3 } }, edt::pick_op::add, false });
	BOOST_TEST((ids() == std::vector<uint64_t>{ 1, 2, 3 }));

	edt::apply_viewport_pick(hierarchy, service, { paths{}, edt::pick_op::add, false });
	edt::apply_viewport_pick(hierarchy, service, { paths{}, edt::pick_op::remove, false });
	BOOST_TEST((ids() == std::vector<uint64_t>{ 1, 2, 3 }));

	edt::apply_viewport_pick(hierarchy, service, { paths{ { 2 } }, edt::pick_op::replace, false });
	BOOST_TEST((ids() == std::vector<uint64_t>{ 2 }));

	edt::apply_viewport_pick(hierarchy, service, { paths{}, edt::pick_op::replace, false });
	BOOST_TEST(service.current().empty());

	edt::apply_viewport_pick(hierarchy, service, { paths{ { 1 } }, edt::pick_op::replace, false });
	edt::apply_viewport_pick(hierarchy, service, { paths{}, edt::pick_op::replace, true });
	BOOST_TEST(service.current().empty());
}

BOOST_AUTO_TEST_SUITE_END()
