#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/level/scn_level_desc.h>
#include "edt_level_controller.h"
#include "edt_prefab_document.h"
#include "edt_scene_editor.h"
#include "edt_scene_document.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// document_manager (#137): the one scene document, the at most one prefab preview, the active document,
// and scene_document::commit as the single place an edit marks the document changed.

namespace {

constexpr const char* PREVIEW_TEMPLATE = R"({
	"__type": "world_desc", "__version": 3, "name": "__prefab_preview", "systems": [],
	"components": { "anchor": { "__type": "anchor_desc", "__version": 1 } },
	"children": [
		{ "__id": "09705f78a49797f0", "name": "PrefabLight",
		  "components": { "directional_light_desc": { "__type": "directional_light_desc", "__version": 1 } } }
	]
})";

constexpr const char* PREFAB = R"({
	"__type": "prefab_desc", "__version": 2,
	"components": { "stat": { "__type": "stat_desc", "hp": 10 } },
	"children": [
		{ "__id": "00000000000000a1", "name": "arm", "components": { "stat": { "__type": "stat_desc", "hp": 3 } } }
	]
})";

struct document_fixture : test_util::scene_env
{
	document_fixture()
	{
		rs->unregistrate_resolver(res::tag::default_protocol());
		auto& res_ram = rs->registrate_resolver<test_util::ram_resolver>(res::tag::default_protocol());
		res_ram.store(res::tag::make("templates/prefab_preview.desc"), test_util::to_bytes(PREVIEW_TEMPLATE));
		res_ram.store(prefab_a, test_util::to_bytes(PREFAB));
		res_ram.store(prefab_b, test_util::to_bytes(PREFAB));

		hot = std::make_shared<scn::hot_reload_manager>(*assembler, *rs);
		mgr = std::make_shared<scn::context_manager>(*rs, *assembler, sf, hot);
		services.ctx_manager = mgr;
		services.assembler = assembler.get();
		services.sfactory = &sf;

		connections.push_back(events.document_opened.subscribe([this](edt::document_id id) { log.push_back("opened:" + std::to_string(id)); }));
		connections.push_back(events.document_closed.subscribe([this](edt::document_id id) { log.push_back("closed:" + std::to_string(id)); }));
		connections.push_back(events.document_activated.subscribe([this](edt::document_id id) { log.push_back("activated:" + std::to_string(id)); }));
		connections.push_back(events.document_changed.subscribe([this](edt::document_id id, std::uint64_t generation) {
			changed.push_back({ id, generation });
		}));
	}

	std::size_t count_documents()
	{
		std::size_t count = 0;
		documents.for_each([&count](edt::document&) { ++count; });
		return count;
	}

	const res::tag prefab_a = res::tag::make("prefabs/a.prefab.desc");
	const res::tag prefab_b = res::tag::make("prefabs/b.prefab.desc");
	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	std::shared_ptr<scn::context_manager> mgr;
	edt::editor_services services;
	edt::scene_document scene;
	edt::editor_events events;
	edt::document_manager documents{ scene, services, events, *rs, *descs };
	std::vector<edt::connection> connections;
	std::vector<std::string> log;
	std::vector<std::pair<edt::document_id, std::uint64_t>> changed;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtDocumentTests, document_fixture)

BOOST_AUTO_TEST_CASE(Scene_IsTheOnlyDocumentAndTheActiveOne)
{
	BOOST_TEST(&documents.scene() == &scene);
	BOOST_TEST(scene.id() != 0u);
	BOOST_TEST(scene.kind() == "scene");
	BOOST_TEST(documents.active() == &scene);
	BOOST_TEST(documents.find(scene.id()) == &scene);
	BOOST_TEST(documents.preview() == nullptr);
	BOOST_TEST(count_documents() == 1u);
}

BOOST_AUTO_TEST_CASE(OpenPrefab_AddsAPreviewAndMakesItActive)
{
	auto& prefab = documents.open_prefab(prefab_a);

	BOOST_REQUIRE(prefab.is_open());
	BOOST_TEST(prefab.id() != 0u);
	BOOST_TEST(prefab.id() != scene.id());
	BOOST_TEST(prefab.kind() == "prefab");
	BOOST_TEST(!prefab.can_save());
	BOOST_TEST(!prefab.is_dirty());
	BOOST_TEST((prefab.source() == prefab_a));
	BOOST_TEST(documents.preview() == &prefab);
	BOOST_TEST(documents.active() == &prefab);
	BOOST_TEST(count_documents() == 2u);
	BOOST_TEST((log == std::vector<std::string>{
		"opened:" + std::to_string(prefab.id()), "activated:" + std::to_string(prefab.id()) }));
}

BOOST_AUTO_TEST_CASE(PrefabDocument_CannotBeSaved)
{
	auto& prefab = documents.open_prefab(prefab_a);
	BOOST_REQUIRE(prefab.is_open());

	BOOST_TEST(!prefab.save().saved);
	BOOST_TEST(!prefab.save_as(prefab_b).saved);
}

BOOST_AUTO_TEST_CASE(OpenPrefab_OwnsThePreviewSetAndCloseDestroysIt)
{
	auto& prefab = documents.open_prefab(prefab_a);
	BOOST_REQUIRE(prefab.is_open());
	const std::string set_name = prefab.set_name();
	BOOST_TEST(set_name == "preview:" + std::string{ prefab_a.view() });
	BOOST_TEST(mgr->find_set(set_name) != nullptr);

	documents.close(prefab.id());

	BOOST_TEST(mgr->find_set(set_name) == nullptr);
	BOOST_TEST(documents.preview() == nullptr);
}

BOOST_AUTO_TEST_CASE(OpenPrefab_ASecondOneReplacesTheFirst)
{
	const auto first = documents.open_prefab(prefab_a).id();
	log.clear();

	auto& second = documents.open_prefab(prefab_b);

	BOOST_REQUIRE(second.is_open());
	BOOST_TEST(second.id() != first);
	BOOST_TEST(documents.find(first) == nullptr);
	BOOST_TEST(count_documents() == 2u);
	BOOST_TEST((log == std::vector<std::string>{
		"activated:" + std::to_string(scene.id()), "closed:" + std::to_string(first),
		"opened:" + std::to_string(second.id()), "activated:" + std::to_string(second.id()) }));
}

BOOST_AUTO_TEST_CASE(Close_Preview_ReturnsTheActiveDocumentToTheScene)
{
	const auto id = documents.open_prefab(prefab_a).id();
	log.clear();

	documents.close(id);

	BOOST_TEST(documents.active() == &scene);
	BOOST_TEST(count_documents() == 1u);
	BOOST_TEST((log == std::vector<std::string>{
		"activated:" + std::to_string(scene.id()), "closed:" + std::to_string(id) }));
}

BOOST_AUTO_TEST_CASE(Close_TheSceneOrAnUnknownIdChangesNothing)
{
	const auto id = documents.open_prefab(prefab_a).id();
	log.clear();

	documents.close(scene.id());
	documents.close(id + 100);

	BOOST_TEST(log.empty());
	BOOST_TEST(count_documents() == 2u);
	BOOST_TEST(documents.active() != nullptr);
}

BOOST_AUTO_TEST_CASE(SetActive_SwitchesBetweenOpenDocumentsAndIgnoresOthers)
{
	const auto id = documents.open_prefab(prefab_a).id();
	log.clear();

	documents.set_active(scene.id());
	BOOST_TEST(documents.active() == &scene);
	documents.set_active(scene.id()); // already active: no event
	documents.set_active(id + 100);
	BOOST_TEST(documents.active() == &scene);
	BOOST_TEST((log == std::vector<std::string>{ "activated:" + std::to_string(scene.id()) }));
}

BOOST_AUTO_TEST_CASE(OpenPrefab_OfAMissingPrefabOpensNothing)
{
	auto& prefab = documents.open_prefab(res::tag::make("prefabs/missing.prefab.desc"));

	BOOST_TEST(!prefab.is_open());
	BOOST_TEST(documents.preview() == nullptr);
	BOOST_TEST(documents.active() == &scene);
	BOOST_TEST(count_documents() == 1u);
	BOOST_TEST(log.empty());
}

BOOST_AUTO_TEST_CASE(ForEach_VisitsTheSceneFirst)
{
	documents.open_prefab(prefab_a);

	std::vector<std::string> kinds;
	documents.for_each([&kinds](edt::document& doc) { kinds.emplace_back(doc.kind()); });

	BOOST_TEST((kinds == std::vector<std::string>{ "scene", "prefab" }));
}

BOOST_AUTO_TEST_CASE(Commit_MarksTheSceneDirtyBumpsTheGenerationAndTellsDocumentChanged)
{
	int pushes = 0;
	scene.set_pusher([&pushes] { ++pushes; return true; });
	BOOST_TEST(!scene.is_dirty());
	const auto before = scene.generation();

	scene.commit("Create entity");

	BOOST_TEST(scene.is_dirty());
	BOOST_TEST(scene.generation() > before);
	BOOST_TEST(pushes == 1);
	BOOST_REQUIRE(changed.size() == 1u);
	BOOST_TEST(changed[0].first == scene.id());
	BOOST_TEST(changed[0].second == scene.generation());
}

BOOST_AUTO_TEST_CASE(Commit_WithNothingToPushIsDirtyButSilent)
{
	scene.set_pusher([] { return false; }); // no level loaded: the push bails out

	scene.commit("Create entity");

	BOOST_TEST(scene.is_dirty());
	BOOST_TEST(changed.empty());
}

BOOST_AUTO_TEST_CASE(Commit_StateOnlyNeitherPushesNorAnnounces)
{
	int pushes = 0;
	scene.set_pusher([&pushes] { ++pushes; return true; });

	scene.commit("Create world", edt::commit_mode::state_only);

	BOOST_TEST(pushes == 0);
	BOOST_TEST(scene.is_dirty());
	BOOST_TEST(changed.empty());
}

BOOST_AUTO_TEST_CASE(Commit_AddsExactlyOneToTheGeneration)
{
	scene.set_pusher([] { return true; });
	const auto before = scene.generation();

	scene.commit("Create entity");
	BOOST_TEST(scene.generation() == before + 1);

	scene.commit("Create world", edt::commit_mode::state_only);
	BOOST_TEST(scene.generation() == before + 2);
}

BOOST_AUTO_TEST_CASE(SceneEditorEdit_ReachesDocumentChangedThroughTheInstalledPusher)
{
	const auto tag = put("lv.desc", R"({"__type":"level_desc","name":"lv","worlds":[
		{"__type":"world_desc","name":"w0","systems":[]}]})");
	BOOST_REQUIRE(mgr->load_level(tag));
	auto level_res = require_loaded<scn::level_desc>(tag);
	BOOST_REQUIRE(level_res.is_ready());
	for (const auto& wh : level_res->get_worlds()) scene.world_descs.push_back(*wh.get_sync());
	edt::derive_world_arrays(scene.world_descs, scene.world_names, scene.worlds_systems_list);
	scene.active_world_idx = 0;
	scene.editor_tag = tag;
	edt::scene_editor editor(scene, services);
	const auto before = scene.generation();
	BOOST_REQUIRE(scene.active_world_desc());

	editor.create_entity("Empty");

	BOOST_TEST(scene.active_world_desc()->get_root().children.size() == 1u);
	BOOST_TEST(scene.is_dirty());
	BOOST_REQUIRE(changed.size() == 1u);
	BOOST_TEST(changed[0].first == scene.id());
	BOOST_TEST(changed[0].second == scene.generation());
	// One edit, one step: the edit method does not bump the generation besides commit().
	BOOST_TEST(scene.generation() == before + 1);
}

BOOST_AUTO_TEST_CASE(SceneEditorEdit_ThatDoesNothing_LeavesTheGenerationAndTheDocumentAlone)
{
	edt::scene_editor editor(scene, services);
	const auto before = scene.generation();
	BOOST_REQUIRE(!scene.active_world_desc());

	editor.create_entity("Empty");
	editor.delete_entity(1);
	editor.duplicate_entity(1);
	editor.reparent_entity(1, 2, -1);

	BOOST_TEST(scene.generation() == before);
	BOOST_TEST(!scene.is_dirty());
	BOOST_TEST(changed.empty());
}

BOOST_AUTO_TEST_CASE(Shutdown_ClosesThePreviewAndLetsGoOfTheScene)
{
	const auto id = documents.open_prefab(prefab_a).id();
	const std::string set_name = "preview:" + std::string{ prefab_a.view() };
	BOOST_REQUIRE(mgr->find_set(set_name));

	documents.shutdown();

	BOOST_TEST(mgr->find_set(set_name) == nullptr);
	BOOST_TEST(documents.find(id) == nullptr);
	BOOST_TEST(documents.preview() == nullptr);
	BOOST_TEST(documents.active() == nullptr);
	scene.set_pusher([] { return true; });
	scene.commit("Create entity");
	BOOST_TEST(changed.empty()); // the scene no longer reports to the manager's events
	documents.shutdown(); // a second call is harmless
}

BOOST_AUTO_TEST_CASE(MarkClean_ClearsTheDirtyFlag)
{
	scene.commit("Create world", edt::commit_mode::state_only);
	BOOST_REQUIRE(scene.is_dirty());

	scene.mark_clean();

	BOOST_TEST(!scene.is_dirty());
}

BOOST_AUTO_TEST_CASE(PreviewDocument_IsNeverDirtyAndItsCommitsAreTheScenesAlone)
{
	auto& prefab = documents.open_prefab(prefab_a);
	scene.commit("Create world", edt::commit_mode::state_only);

	BOOST_TEST(scene.is_dirty());
	BOOST_TEST(!prefab.is_dirty());
}

BOOST_AUTO_TEST_CASE(SceneSave_GoesThroughTheHandlersOfTheEditor)
{
	BOOST_TEST(!scene.can_save());
	BOOST_TEST(!scene.save().saved);

	int saves = 0;
	res::tag saved_as;
	scene.set_save_handlers(
		[&saves] { ++saves; return true; },
		[&saved_as](const res::tag& target) { saved_as = target; return true; });
	BOOST_TEST(!scene.can_save()); // no level yet

	scene.editor_tag = res::tag{ "memory://editor/lv.desc" };
	BOOST_REQUIRE(scene.can_save());
	BOOST_TEST(scene.save().saved);
	BOOST_TEST(saves == 1);
	BOOST_TEST(scene.save_as(prefab_b).saved);
	BOOST_TEST((saved_as == prefab_b));
}

BOOST_AUTO_TEST_CASE(SceneSave_ReportsAFailedWrite)
{
	scene.editor_tag = res::tag{ "memory://editor/lv.desc" };
	scene.set_save_handlers([] { return false; }, [](const res::tag&) { return false; });

	const auto result = scene.save();

	BOOST_TEST(!result.saved);
	BOOST_TEST(!result.error.empty());
}

BOOST_AUTO_TEST_CASE(SceneDocument_ExposesItsIdentity)
{
	scene.level_name = "lv";
	scene.level_tag = res::tag::make("levels/lv.desc");
	BOOST_TEST(scene.title() == "lv");
	BOOST_TEST((scene.source() == scene.level_tag));
}

BOOST_AUTO_TEST_CASE(EditorServices_LevelSetIsTheContextManagersAndNullOnceItIsGone)
{
	BOOST_TEST(services.level_set() == &mgr->level_set());

	services.ctx_manager.reset();
	BOOST_TEST(services.level_set() == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
