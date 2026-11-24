#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_node_patch.h"
#include "edt_scene_document.h"
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_node_index.h>
#include <boost/json/serialize.hpp>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_model.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// #1190: a field edit of one node reaches its live entity without a rebuild of the world, and what it gives equals what a
// rebuild of the same descs gives.

namespace {

using prefab_node = scn::prefab_desc::prefab_node;

constexpr std::uint64_t CAM_ID = 0xc1;
constexpr std::uint64_t BOX_ID = 0xc2;

constexpr const char* WORLD = R"({
	"__type": "world_desc", "__version": 5, "name": "w0", "systems": [],
	"children": [
		{ "__id": "00000000000000c1", "name": "Cam",
		  "components": { "camera_desc": { "__type": "camera_desc", "__version": 2, "fov": 70.0 } } },
		{ "__id": "00000000000000c2", "name": "Box" }
	]
})";

constexpr const char* PREFAB = R"({ "__type": "prefab_desc", "__version": 5, "name": "rig", "children": [ { "__id": "00000000000000f1", "name": "leg" } ] })";

struct patch_fixture : test_util::scene_env
{
	patch_fixture()
	{
		put("rig.prefab.desc", PREFAB);
		auto loaded = load<scn::world_desc>(put("w0.desc", WORLD));
		BOOST_REQUIRE(loaded);
		world = *loaded;
		scene.world_descs.push_back(world);
		scene.world_names.emplace_back("w0");
		scene.active_world_idx = 0;
		scene.reset_history();
		scene.mark_clean();
		scene.set_pusher([this] { ++pushes; return true; });
		scene.set_world_reloader([this](std::size_t) { ++reloads; return true; });
		root = reg.create();
		scn::spawn_world(*assembler, reg, root, scene.world_descs[0], "w0", res::tag{});
	}

	static prefab_node* node_of(scn::world_desc& desc, std::uint64_t id)
	{
		for (auto& child : desc.get_root().children) {
			if (child.node_id == id) return &child;
		}
		return nullptr;
	}

	prefab_node& node(std::uint64_t id) { return *node_of(scene.world_descs[0], id); }

	entt::entity live(entt::registry& from, std::uint64_t id)
	{
		const std::vector<std::uint64_t> path{ id };
		return scn::node_index::get(from).find(path);
	}

	void edit_fov(float fov) { node(CAM_ID).components["camera_desc"].overrides["fov"] = fov; }

	void install_patcher(bool result)
	{
		scene.set_node_patcher([this, result](std::span<const edt::scene_node_edit> edits) {
			++patches;
			last_edits.assign(edits.begin(), edits.end());
			return result;
		});
	}

	scn::world_desc world;
	edt::scene_document scene;
	entt::registry reg;
	entt::entity root = entt::null;
	int pushes = 0;
	int reloads = 0;
	int patches = 0;
	std::vector<edt::scene_node_edit> last_edits;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtNodePatch, patch_fixture)

BOOST_AUTO_TEST_CASE(AFieldEdit_GivesWhatARebuildGives_OnTheSameEntity)
{
	const entt::entity cam = live(reg, CAM_ID);
	BOOST_REQUIRE(reg.valid(cam));
	const std::size_t entities_before = reg.storage<entt::entity>().size();

	edit_fov(40.f);
	node(CAM_ID).position = { 1.f, 2.f, 3.f };
	node(CAM_ID).has_transform = true;
	node(CAM_ID).name = "Eye";
	const edt::scene_node_edit edit{ 0, CAM_ID, true, true, { "camera_desc" } };
	BOOST_REQUIRE(edt::patch_live_nodes(scene.world_descs[0], reg, *assembler, std::span(&edit, 1)));

	entt::registry rebuilt;
	const auto rebuilt_root = rebuilt.create();
	scn::spawn_world(*assembler, rebuilt, rebuilt_root, scene.world_descs[0], "w0", res::tag{});
	const entt::entity fresh = live(rebuilt, CAM_ID);
	BOOST_REQUIRE(rebuilt.valid(fresh));

	BOOST_TEST((live(reg, CAM_ID) == cam));
	BOOST_TEST(reg.storage<entt::entity>().size() == entities_before);
	BOOST_TEST(reg.get<scn::camera_component>(cam).fov == rebuilt.get<scn::camera_component>(fresh).fov);
	BOOST_TEST(reg.get<scn::camera_component>(cam).fov == 40.f);
	BOOST_TEST((reg.get<scn::local_transform>(cam).local == rebuilt.get<scn::local_transform>(fresh).local));
	BOOST_TEST(reg.get<scn::name_component>(cam).name == rebuilt.get<scn::name_component>(fresh).name);
}

// The descs of the document are the source of truth (a push reloads the world from them, a save writes them): a patch
// never writes anywhere else, so every later rebuild, hot reload and save must still see what it applied.
BOOST_AUTO_TEST_CASE(APatchedField_SurvivesAHotReloadOfItsParentDesc_ARebuild_AndIsSaved)
{
	const res::tag lens_tag = put("lens.desc", R"({ "__type": "camera_desc", "fov": 30.0 })");
	scn::prefab_desc::prefab_comp_node lens;
	lens.type_name = "camera_desc";
	lens.parent_desc = require_loaded<desc::desc_base>(lens_tag);
	lens.overrides["fov"] = 50.0;
	node(BOX_ID).components["lens"] = lens;

	entt::registry live_reg;
	const auto live_root = live_reg.create();
	scn::spawn_world(*assembler, live_reg, live_root, scene.world_descs[0], "w0", res::tag{});
	const entt::entity box = live(live_reg, BOX_ID);
	BOOST_REQUIRE(live_reg.valid(box));
	BOOST_TEST(live_reg.get<scn::camera_component>(box).fov == 50.f);

	node(BOX_ID).components["lens"].overrides["fov"] = 65.0;
	const edt::scene_node_edit edit{ 0, BOX_ID, false, false, { "lens" } };
	BOOST_REQUIRE(edt::patch_live_nodes(scene.world_descs[0], live_reg, *assembler, std::span(&edit, 1)));
	BOOST_TEST(live_reg.get<scn::camera_component>(box).fov == 65.f);

	put("lens.desc", R"({ "__type": "camera_desc", "fov": 31.0 })");
	rs->signal_changed(lens_tag);
	pump();
	assembler->trigger_hot_reload_for_component(live_reg, require_loaded<desc::desc_base>(lens_tag));
	BOOST_TEST(live_reg.get<scn::camera_component>(box).fov == 65.f);

	entt::registry rebuilt;
	const auto rebuilt_root = rebuilt.create();
	scn::spawn_world(*assembler, rebuilt, rebuilt_root, scene.world_descs[0], "w0", res::tag{});
	BOOST_TEST(rebuilt.get<scn::camera_component>(live(rebuilt, BOX_ID)).fov == 65.f);

	const auto saved = scn::level_desc::build_json("lvl", scene.world_descs, *descs);
	BOOST_TEST(boost::json::serialize(saved).find("65") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AnEditOfANodeThatIsNotLive_TouchesNothing_AndIsRefused)
{
	const entt::entity cam = live(reg, CAM_ID);
	edit_fov(40.f);
	const std::vector<edt::scene_node_edit> edits{
		{ 0, CAM_ID, false, false, { "camera_desc" } },
		{ 0, 0xdead, true, false, {} },
	};
	BOOST_TEST(!edt::patch_live_nodes(scene.world_descs[0], reg, *assembler, edits));
	BOOST_TEST(reg.get<scn::camera_component>(cam).fov == 70.f);
}

BOOST_AUTO_TEST_CASE(AFieldEditOfAComponent_GoesToThePatcher_NotToThePusher)
{
	install_patcher(true);
	edit_fov(35.f);
	scene.commit("Edit node");
	BOOST_TEST(patches == 1);
	BOOST_TEST(pushes == 0);
	BOOST_REQUIRE(last_edits.size() == 1u);
	BOOST_TEST(last_edits[0].id == CAM_ID);
	BOOST_TEST(!last_edits[0].transform);
	BOOST_TEST((last_edits[0].components == std::vector<std::string>{ "camera_desc" }));
	BOOST_TEST(scene.is_dirty());
}

BOOST_AUTO_TEST_CASE(ATransformEdit_IsAFieldEdit)
{
	install_patcher(true);
	node(BOX_ID).position = { 0.f, 5.f, 0.f };
	node(BOX_ID).has_transform = true;
	scene.commit("Move entity");
	BOOST_TEST(patches == 1);
	BOOST_TEST(pushes == 0);
	BOOST_REQUIRE(last_edits.size() == 1u);
	BOOST_TEST(last_edits[0].transform);
}

BOOST_AUTO_TEST_CASE(APatcherThatRefuses_LeavesTheRebuildToThePusher)
{
	install_patcher(false);
	edit_fov(35.f);
	scene.commit("Edit node");
	BOOST_TEST(patches == 1);
	BOOST_TEST(pushes == 1);
}

BOOST_AUTO_TEST_CASE(AStructuralEdit_IsNeverPatched)
{
	install_patcher(true);
	prefab_node added;
	added.name = "New";
	added.node_id = 0xc3;
	scene.world_descs[0].get_root().children.push_back(added);
	scene.commit("Create entity");
	BOOST_TEST(patches == 0);
	BOOST_TEST(pushes == 1);
}

BOOST_AUTO_TEST_CASE(AddingOrRemovingAComponent_IsStructural)
{
	install_patcher(true);
	scn::prefab_desc::prefab_comp_node extra;
	extra.type_name = "camera_desc";
	node(BOX_ID).components["camera_desc"] = extra;
	scene.commit("Add component");
	BOOST_TEST(patches == 0);
	BOOST_TEST(pushes == 1);
}

BOOST_AUTO_TEST_CASE(ANodeThatInstantiatesAPrefab_IsAlwaysRebuilt_EvenForItsTransform)
{
	install_patcher(true);
	scn::prefab_desc::prefab_comp_node instance;
	instance.type_name = "prefab_desc";
	instance.parent_desc = require_loaded<desc::desc_base>(res::tag{ "memory://t/rig.prefab.desc" });
	node(BOX_ID).components["inst"] = instance;
	scene.commit("Add instance");
	BOOST_REQUIRE(pushes == 1);

	node(BOX_ID).position = { 3.f, 0.f, 0.f };
	node(BOX_ID).has_transform = true;
	scene.commit("Move entity");
	BOOST_TEST(patches == 0);
	BOOST_TEST(pushes == 2);
}

BOOST_AUTO_TEST_CASE(AnEditOfAWorldThatIsNotActive_IsRebuiltNotPatched)
{
	scene.world_descs.push_back(scene.world_descs[0]);
	scene.world_names.emplace_back("w1");
	scene.reset_history();
	install_patcher(true);
	scene.world_descs[1].get_root().children[0].components["camera_desc"].overrides["fov"] = 20.f;
	scene.commit("Edit node");
	BOOST_TEST(patches == 0);
	BOOST_TEST(reloads == 1);
}

BOOST_AUTO_TEST_CASE(WhileEditsAreLocked_NothingIsPatched)
{
	install_patcher(true);
	scene.set_edits_locked(true);
	edit_fov(35.f);
	scene.commit("Edit node");
	BOOST_TEST(patches == 0);
	BOOST_TEST(pushes == 0);
}

BOOST_AUTO_TEST_SUITE_END()
