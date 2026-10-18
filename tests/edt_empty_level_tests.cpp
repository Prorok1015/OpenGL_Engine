#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/level/scn_level_manager.h>
#include <snk/scene/level/scn_level_desc.h>
#include "edt_level_controller.h"
#include "edt_scene_editor.h"
#include "edt_shared_state.h"
#include "edt_world_controller.h"
#include <chrono>

// A level with no worlds is a valid editor state (active_world_idx = -1): the accessors, the world arrays
// after a reload, the save and the scene editor must cope with it.

namespace {

constexpr const char* LEVEL_TWO_WORLDS = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":[]},
	{"__type":"world_desc","name":"w1","systems":[]}]})";

constexpr const char* LEVEL_NO_WORLDS = R"({"__type":"level_desc","name":"lv","worlds":[]})";

struct empty_level_fixture : test_util::scene_env
{
	empty_level_fixture()
		: hot(std::make_shared<scn::hot_reload_manager>(*assembler, *rs))
		, mgr(*rs, *assembler, sf, hot)
	{}

	// What level_controller::populate_worlds_from_level does, on the editor's side of the state.
	void populate(const res::tag& tag)
	{
		auto level_res = rs->require_sync<scn::level_desc>(tag);
		BOOST_REQUIRE(level_res.is_ready());
		state.world_descs.clear();
		for (const auto& wh : level_res->get_worlds()) state.world_descs.push_back(*wh.get_sync());
		edt::derive_world_arrays(state.world_descs, state.world_names, state.worlds_systems_list);
		state.active_world_idx = state.world_names.empty() ? -1 : 0;
	}

	// level_controller::resync_worlds_after_reload minus the controller's own plumbing: it runs the real
	// edt::apply_world_resync on the reloaded level desc.
	void resync()
	{
		scn::level& lvl = mgr.get_level();
		auto level_res = rs->require_sync<scn::level_desc>(lvl.get_tag());
		BOOST_REQUIRE(level_res.is_ready());
		std::vector<scn::world_desc> desc_worlds;
		for (const auto& wh : level_res->get_worlds()) desc_worlds.push_back(*wh.get_sync());
		edt::apply_world_resync(state, world_ctrl, desc_worlds, !level_res->get_worlds().empty(),
			[&lvl](const scn::world_desc& w) { return lvl.has_world(w.get_name()); });
	}

	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	scn::level_manager mgr;
	edt::shared_state state;
	edt::world_controller world_ctrl{ state, *rs, *descs };
};

}

BOOST_FIXTURE_TEST_SUITE(EdtEmptyLevelTests, empty_level_fixture)

BOOST_AUTO_TEST_CASE(ActiveWorldAccessors_ReturnNullInsteadOfIndexingOutOfBounds)
{
	BOOST_TEST(state.active_world_desc() == nullptr); // idx 0, no world
	BOOST_TEST(state.active_world_name() == nullptr);
	BOOST_TEST(!state.has_active_world());

	state.world_descs.emplace_back();
	state.world_names.push_back("w");
	BOOST_TEST(state.active_world_desc() == &state.world_descs[0]);
	BOOST_REQUIRE(state.active_world_name());
	BOOST_TEST(*state.active_world_name() == "w");

	state.active_world_idx = -1;
	BOOST_TEST(state.active_world_desc() == nullptr);
	BOOST_TEST(state.active_world_name() == nullptr);
	state.active_world_idx = 1;
	BOOST_TEST(state.active_world_desc() == nullptr);
}

BOOST_AUTO_TEST_CASE(Reload_ToAnEmptyWorldListClearsTheEditorArrays)
{
	const auto tag = put("lv.desc", LEVEL_TWO_WORLDS);
	BOOST_REQUIRE(mgr.load(tag));
	populate(tag);
	state.active_world_idx = 1;
	BOOST_REQUIRE(state.active_world_desc());

	int reloads = 0;
	int switched_to = 0;
	world_ctrl.on_world_switched = [&switched_to](int idx) { switched_to = idx; };
	mgr.set_on_reload([&] { ++reloads; resync(); });
	put("lv.desc", LEVEL_NO_WORLDS);
	rs->signal_changed(tag);
	mgr.update(std::chrono::duration<float>(0.001f));

	BOOST_TEST(reloads == 1);
	BOOST_TEST(switched_to == -1); // the panels were told there is no world
	BOOST_TEST(state.active_world_idx == -1);
	BOOST_TEST(state.world_descs.empty());
	BOOST_TEST(state.world_names.empty());
	BOOST_TEST(state.worlds_systems_list.empty());
	BOOST_TEST(state.active_world_desc() == nullptr);
	BOOST_TEST(mgr.get_level().get_world_count() == 0u);
}

BOOST_AUTO_TEST_CASE(Reload_FromAnEmptyListToWorldsPicksTheFirstWorld)
{
	const auto tag = put("lv.desc", LEVEL_NO_WORLDS);
	BOOST_REQUIRE(mgr.load(tag));
	populate(tag);
	BOOST_TEST(state.active_world_idx == -1);

	mgr.set_on_reload([&] { resync(); });
	put("lv.desc", LEVEL_TWO_WORLDS);
	rs->signal_changed(tag);
	mgr.update(std::chrono::duration<float>(0.001f));

	BOOST_TEST(state.active_world_idx == 0);
	BOOST_REQUIRE(state.world_names.size() == 2u);
	BOOST_TEST(state.world_names[0] == "w0");
	BOOST_TEST(state.active_world_desc() == &state.world_descs[0]);
}

BOOST_AUTO_TEST_CASE(Resync_TellsThePanelsEvenWhenTheSwitchIsRefused)
{
	const auto tag = put("lv.desc", LEVEL_TWO_WORLDS);
	BOOST_REQUIRE(mgr.load(tag));
	populate(tag);
	state.active_world_idx = 1; // "w1", which the reloaded desc no longer has

	std::vector<int> switched;
	world_ctrl.on_world_switched = [&switched](int idx) { switched.push_back(idx); };
	const uint64_t generation = state.desc_generation;
	std::vector<scn::world_desc> only_w0{ state.world_descs[0] };

	// no level manager is attached to the state, so switch_to_world itself refuses
	edt::apply_world_resync(state, world_ctrl, only_w0, true, [](const scn::world_desc&) { return false; });

	BOOST_TEST((switched == std::vector<int>{ 0 }));
	BOOST_TEST(state.active_world_idx == 0);
	BOOST_TEST(state.world_names.size() == 1u);
	BOOST_TEST(state.desc_generation != generation);
}

BOOST_AUTO_TEST_CASE(Resync_KeepsTheStateWhenTheDescListsWorldsButNoneLoaded)
{
	const auto tag = put("lv.desc", LEVEL_TWO_WORLDS);
	BOOST_REQUIRE(mgr.load(tag));
	populate(tag);
	int switches = 0;
	world_ctrl.on_world_switched = [&switches](int) { ++switches; };

	edt::apply_world_resync(state, world_ctrl, {}, true, [](const scn::world_desc&) { return false; });

	BOOST_TEST(state.world_names.size() == 2u);
	BOOST_TEST(state.active_world_idx == 0);
	BOOST_TEST(switches == 0);
}

BOOST_AUTO_TEST_CASE(SelectWorld_RefusedSwitchKeepsTheOldWorldAndTellsThePanels)
{
	populate(put("lv.desc", LEVEL_TWO_WORLDS)); // no level manager is attached, so every switch is refused
	BOOST_REQUIRE(state.world_names.size() == 2u);
	state.active_world_idx = 0;

	std::vector<int> switched;
	world_ctrl.on_world_switched = [&switched](int idx) { switched.push_back(idx); };
	const uint64_t generation = state.desc_generation;

	BOOST_TEST(!world_ctrl.switch_to_world(1));
	BOOST_TEST(state.active_world_idx == 0);

	world_ctrl.select_world(1);

	BOOST_TEST(state.active_world_idx == 0);
	BOOST_TEST((switched == std::vector<int>{ 0 }));
	BOOST_TEST(state.desc_generation != generation);
	BOOST_TEST(state.world_descs.size() == 2u);
	BOOST_TEST(state.worlds_systems_list.size() == 2u);
	BOOST_REQUIRE(state.active_world_desc());
	BOOST_TEST(state.active_world_desc() == &state.world_descs[0]);
}

BOOST_AUTO_TEST_CASE(SelectWorld_RefusedSwitchClearsAnIndexThatIsOutOfRange)
{
	populate(put("lv.desc", LEVEL_TWO_WORLDS));
	std::vector<int> switched;
	world_ctrl.on_world_switched = [&switched](int idx) { switched.push_back(idx); };

	for (const int stale : { -1, 5 }) {
		state.active_world_idx = stale;
		world_ctrl.select_world(1);
		BOOST_TEST(state.active_world_idx == -1);
		BOOST_TEST(state.active_world_desc() == nullptr);
	}
	BOOST_TEST((switched == std::vector<int>{ -1, -1 }));
}

BOOST_AUTO_TEST_CASE(SceneEditor_SyncEcsTransformsToDescBumpsTheDescGeneration)
{
	const auto tag = put("lv.desc", LEVEL_TWO_WORLDS);
	auto shared_mgr = std::make_shared<scn::level_manager>(*rs, *assembler, sf, hot);
	BOOST_REQUIRE(shared_mgr->load(tag));
	populate(tag);
	state.lvl_manager = shared_mgr;
	edt::scene_editor editor(state);

	const uint64_t generation = state.desc_generation;
	editor.sync_ecs_transforms_to_desc();

	BOOST_TEST(state.desc_generation != generation);
}

BOOST_AUTO_TEST_CASE(Save_WithoutWorldsWritesAnEmptyWorldList)
{
	const auto json = scn::level_desc::build_json("lv", state.world_descs, *descs, "");
	const auto* worlds = json.if_contains("worlds");
	BOOST_REQUIRE(worlds);
	BOOST_REQUIRE(worlds->is_array());
	BOOST_TEST(worlds->as_array().empty());
}

BOOST_AUTO_TEST_CASE(SceneEditor_EditsWithoutAnActiveWorldDoNothing)
{
	for (const int idx : { 0, -1 }) {
		state.active_world_idx = idx; // both the default and the explicit "no world"
		edt::scene_editor editor(state);
		int modified = 0;
		editor.on_before_modify = [&modified] { ++modified; };

		const std::vector<uint64_t> path{ 0x1ull };
		BOOST_CHECK_NO_THROW(editor.create_entity("Empty"));
		BOOST_CHECK_NO_THROW(editor.create_entity_from_node({}));
		BOOST_CHECK_NO_THROW(editor.delete_entity(0x1ull));
		BOOST_CHECK_NO_THROW(editor.delete_entities({ 0x1ull }));
		BOOST_CHECK_NO_THROW(editor.duplicate_entity(0x1ull));
		BOOST_CHECK_NO_THROW(editor.duplicate_entities({ 0x1ull }));
		BOOST_CHECK_NO_THROW(editor.reparent_entity(0x1ull, 0x2ull, -1));
		BOOST_CHECK_NO_THROW(editor.on_transform_committed(path, glm::vec3{ 0 }, glm::vec3{ 0 }, glm::vec3{ 1 }));
		BOOST_CHECK_NO_THROW(editor.sync_ecs_transforms_to_desc());
		BOOST_CHECK_NO_THROW(editor.serialize_and_push());
		BOOST_TEST(editor.make_unique_key("Entity") == "Entity");
		BOOST_TEST(modified == 0);
		BOOST_TEST(state.world_descs.empty());
	}
}

BOOST_AUTO_TEST_SUITE_END()
