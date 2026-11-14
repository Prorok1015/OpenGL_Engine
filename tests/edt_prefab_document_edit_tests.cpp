#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_prefab_document.h"
#include <snk/editor/edt_editor_services.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_model.h>
#include <entt/entt.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// #147 (EPIC-25 US-25-2): the prefab document keeps a working copy that edit() changes, Apply (save()) writes over the
// file and revert() takes from the file again.

namespace {

namespace fs = std::filesystem;

constexpr const char* PREVIEW_TEMPLATE = R"({
	"__type": "world_desc", "__version": 4, "name": "__prefab_preview", "systems": [],
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

struct prefab_edit_fixture : test_util::scene_env
{
	prefab_edit_fixture()
	{
		fs::create_directories(res_root / "prefabs");
		fs::create_directories(lower / "prefabs");
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		auto& edt_ram = rs->registrate_resolver<test_util::ram_resolver>("edt");
		edt_ram.store(res::tag("edt://templates/prefab_preview.desc"), test_util::to_bytes(PREVIEW_TEMPLATE));
		write(res_root, "prefabs/a.prefab.desc", PREFAB);

		hot = std::make_shared<scn::hot_reload_manager>(*assembler, *rs);
		mgr = std::make_shared<scn::context_manager>(*rs, *assembler, sf, hot);
		services.ctx_manager = mgr;
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;
		document = std::make_unique<edt::prefab_document>(services);
		document->set_files(layout());
	}

	~prefab_edit_fixture()
	{
		document.reset();
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	edt::desc_file_layout layout() const
	{
		edt::desc_file_layout out;
		out.top_layer = [this] { return res_root; };
		out.layers = [this] { return std::vector<fs::path>{ lower, res_root }; };
		out.open_level_files = [] { return std::vector<std::string>{}; };
		return out;
	}

	static void write(const fs::path& root, std::string_view name, std::string_view content)
	{
		std::ofstream out(root / std::string(name), std::ios::binary);
		out << content;
	}

	std::string text_of(const fs::path& root, std::string_view name) const
	{
		std::ifstream in(root / std::string(name), std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	bool rename_arm(const std::string& name)
	{
		return document->edit([&name](scn::prefab_desc& prefab) {
			prefab.get_root().children[0].name = name;
			return true;
		});
	}

	std::string arm_name() const { return document->working()->get_root().children[0].name; }

	bool preview_has_node_named(const std::string& name)
	{
		entt::registry* reg = document->get_registry();
		if (!reg) return false;
		for (auto&& [entity, component] : reg->view<scn::name_component>().each()) {
			if (component.name == name) return true;
		}
		return false;
	}

	test_util::temp_dir dir{ "edt_prefab_edit" };
	const fs::path res_root = dir.path() / "res";
	const fs::path lower = dir.path() / "engine";
	const res::tag tag = res::tag::make("prefabs/a.prefab.desc");
	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	std::shared_ptr<scn::context_manager> mgr;
	edt::editor_services services;
	std::unique_ptr<edt::prefab_document> document;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtPrefabDocumentEditTests, prefab_edit_fixture)

BOOST_AUTO_TEST_CASE(Open_MakesAWorkingCopy_ThatIsNotDirty)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(document->working() != nullptr);
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(document->can_save());
	BOOST_TEST(arm_name() == "arm");
}

BOOST_AUTO_TEST_CASE(Edit_ChangesTheWorkingCopyAndThePreview_NotTheCacheOrTheFile)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	const std::string before = text_of(res_root, "prefabs/a.prefab.desc");
	const std::uint64_t generation = document->generation();

	BOOST_REQUIRE(rename_arm("arm2"));

	BOOST_TEST(document->is_dirty());
	BOOST_TEST(document->generation() > generation);
	BOOST_TEST(arm_name() == "arm2");
	BOOST_TEST(preview_has_node_named("arm2"));
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc") == before);
	auto cached = rs->require_sync<scn::prefab_desc>(tag);
	BOOST_REQUIRE(cached.is_ready());
	BOOST_TEST(cached->get_root().children[0].name == "arm");
}

BOOST_AUTO_TEST_CASE(Edit_BackToWhatTheFileHolds_IsNotDirty)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));
	BOOST_REQUIRE(document->is_dirty());
	BOOST_REQUIRE(rename_arm("arm"));
	BOOST_TEST(!document->is_dirty());
}

BOOST_AUTO_TEST_CASE(Edit_ThatChangesNothing_IsRefused)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	const std::uint64_t generation = document->generation();
	BOOST_TEST(!document->edit([](scn::prefab_desc&) { return false; }));
	BOOST_TEST(document->generation() == generation);
	BOOST_TEST(!document->is_dirty());
}

BOOST_AUTO_TEST_CASE(Apply_WritesTheFile_AndTheDocumentIsCleanAgain)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));

	const edt::save_result saved = document->save();
	BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);

	BOOST_TEST(!document->is_dirty());
	const std::string written = text_of(res_root, "prefabs/a.prefab.desc");
	BOOST_TEST(written.find("arm2") != std::string::npos);
	BOOST_TEST(written.find("\"prefab_desc\"") != std::string::npos);
	// Nothing to write now: a second Apply is no write and no error.
	BOOST_TEST(document->save().saved);
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc") == written);
}

BOOST_AUTO_TEST_CASE(Apply_AfterAnApply_AnotherEditCanBeAppliedToo)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));
	BOOST_REQUIRE(document->save().saved);
	BOOST_REQUIRE(rename_arm("arm3"));
	BOOST_REQUIRE_MESSAGE(document->save().saved, "the second Apply");
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc").find("arm3") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Apply_ToAFileThatChangedOnDisk_IsRefusedAndWritesNothing)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));
	write(res_root, "prefabs/a.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000a1","name":"other"}]})");
	const std::string outside = text_of(res_root, "prefabs/a.prefab.desc");

	const edt::save_result saved = document->save();

	BOOST_TEST(!saved.saved);
	BOOST_TEST(saved.error.find("changed on disk") != std::string::npos);
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc") == outside);
	BOOST_TEST(document->is_dirty());
}

BOOST_AUTO_TEST_CASE(Apply_OfAPrefabOfALowerLayer_IsRefusedAndWritesNothing)
{
	write(lower, "prefabs/low.prefab.desc", PREFAB);
	fs::remove(res_root / "prefabs" / "a.prefab.desc");
	const res::tag low = res::tag::make("prefabs/low.prefab.desc");
	document->open(low);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(document->edit([](scn::prefab_desc& prefab) {
		prefab.get_root().children[0].name = "arm2";
		return true;
	}));

	const edt::save_result saved = document->save();

	BOOST_TEST(!saved.saved);
	BOOST_TEST(saved.error.find("lower layer") != std::string::npos);
	BOOST_TEST(!fs::exists(res_root / "prefabs" / "low.prefab.desc"));
}

BOOST_AUTO_TEST_CASE(Apply_WhileTheProjectMayNotBeWritten_IsRefusedWithThatReason)
{
	edt::desc_file_layout files = layout();
	files.write_blocker = []() -> std::optional<std::string> { return "a project migration is running"; };
	document->set_files(std::move(files));
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));
	const std::string before = text_of(res_root, "prefabs/a.prefab.desc");

	const edt::save_result saved = document->save();

	BOOST_TEST(!saved.saved);
	BOOST_TEST(saved.error == "a project migration is running");
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc") == before);
}

BOOST_AUTO_TEST_CASE(Play_LocksTheDocument_NoEditAndNoApply)
{
	bool playing = false;
	document->set_lock_source([&playing] { return playing; });
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));

	playing = true;
	BOOST_TEST(document->edits_locked());
	BOOST_TEST(!rename_arm("arm3"));
	BOOST_TEST(arm_name() == "arm2");
	BOOST_TEST(!document->save().saved);
	BOOST_TEST(!document->revert().empty());

	playing = false;
	BOOST_TEST(document->save().saved);
}

BOOST_AUTO_TEST_CASE(Apply_WithoutKnowingTheProjectFiles_WritesNothing)
{
	edt::prefab_document bare(services);
	bare.open(tag);
	BOOST_REQUIRE(bare.is_open());
	BOOST_REQUIRE(bare.edit([](scn::prefab_desc& prefab) {
		prefab.get_root().children[0].name = "arm2";
		return true;
	}));
	const std::string before = text_of(res_root, "prefabs/a.prefab.desc");

	BOOST_TEST(!bare.save().saved);
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc") == before);
	bare.close();
}

BOOST_AUTO_TEST_CASE(Revert_PutsWhatTheFileHoldsBack_AndNothingIsWritten)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));
	const std::string before = text_of(res_root, "prefabs/a.prefab.desc");
	const std::uint64_t generation = document->generation();

	const std::string error = document->revert();

	BOOST_TEST_REQUIRE(error.empty(), error);
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(arm_name() == "arm");
	BOOST_TEST(preview_has_node_named("arm"));
	BOOST_TEST(document->generation() > generation);
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc") == before);
}

BOOST_AUTO_TEST_CASE(Revert_TakesTheFileAsItIsNow_NotTheCachedRead)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	write(res_root, "prefabs/a.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000a1","name":"outside"}]})");

	const std::string error = document->revert();

	BOOST_TEST_REQUIRE(error.empty(), error);
	BOOST_TEST(arm_name() == "outside");
	BOOST_TEST(!document->is_dirty());
	// The file the document now stands on is the new one: an Apply after an edit is not refused as "changed on disk".
	BOOST_REQUIRE(rename_arm("arm2"));
	BOOST_TEST(document->save().saved);
}

BOOST_AUTO_TEST_CASE(Apply_KeepsTheRootGuid)
{
	write(res_root, "prefabs/g.prefab.desc", R"({"__type":"prefab_desc","__version":2,"__guid":"00000000-0000-0000-0000-00000000abcd","children":[{"__id":"00000000000000a1","name":"arm"}]})");
	document->open(res::tag::make("prefabs/g.prefab.desc"));
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));

	BOOST_TEST_REQUIRE(document->save().saved);

	BOOST_TEST(text_of(res_root, "prefabs/g.prefab.desc").find("00000000-0000-0000-0000-00000000abcd") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Apply_OfAVariant_KeepsItsParent_AndWritesOnlyTheDifferences)
{
	write(res_root, "prefabs/base.prefab.desc", PREFAB);
	write(res_root, "prefabs/v.prefab.desc", R"({"__type":"prefab_desc","__version":2,"__parent":"res://prefabs/base.prefab.desc"})");
	document->open(res::tag::make("prefabs/v.prefab.desc"));
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));

	BOOST_TEST_REQUIRE(document->save().saved);

	const std::string file = text_of(res_root, "prefabs/v.prefab.desc");
	BOOST_TEST(file.find("__parent") != std::string::npos);
	BOOST_TEST(file.find("arm2") != std::string::npos);
	BOOST_TEST(file.find("\"hp\"") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(Edit_OfTheRootTransform_ReachesThePreview)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(document->edit([](scn::prefab_desc& prefab) {
		prefab.get_root().has_transform = true;
		prefab.get_root().position = { 4.f, 5.f, 6.f };
		return true;
	}));
	entt::registry* reg = document->get_registry();
	BOOST_REQUIRE(reg);
	bool found = false;
	for (auto&& [entity, placed] : reg->view<scn::local_transform>().each()) {
		if (placed.local[3].x == 4.f && placed.local[3].y == 5.f && placed.local[3].z == 6.f) found = true;
	}
	BOOST_TEST(found);
}

BOOST_AUTO_TEST_CASE(Apply_OfTheOpenLevelsFile_IsRefused)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::desc_file_layout files = layout();
	files.open_level_files = [] { return std::vector<std::string>{ "prefabs/a.prefab.desc" }; };
	document->set_files(files);
	BOOST_REQUIRE(rename_arm("arm2"));
	const std::string before = text_of(res_root, "prefabs/a.prefab.desc");

	const edt::save_result saved = document->save();

	BOOST_TEST(!saved.saved);
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc") == before);
	BOOST_TEST(document->is_dirty());
}

BOOST_AUTO_TEST_CASE(Revert_OfADeletedFile_ReportsItAndKeepsTheEdits)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));
	fs::remove(res_root / "prefabs/a.prefab.desc");

	const std::string error = document->revert();

	BOOST_TEST(!error.empty());
	BOOST_TEST(arm_name() == "arm2");
	BOOST_TEST(document->is_dirty());
}

BOOST_AUTO_TEST_CASE(Close_DropsTheWorkingCopy)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	document->close();
	BOOST_TEST(document->working() == nullptr);
	BOOST_TEST(!document->is_dirty());
}

BOOST_AUTO_TEST_SUITE_END()
