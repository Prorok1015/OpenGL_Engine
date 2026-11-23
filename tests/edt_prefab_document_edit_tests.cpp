#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_prefab_document.h"
#include "edt_prefab_file_edit.h"
#include "edt_prefab_variant_creator.h"
#include "edt_desc_problems.h"
#include "edt_override_edit.h"
#include "edt_instance_overrides_view.h"
#include "edt_inspector_target.h"
#include "edt_scene_document.h"
#include "edt_prefab_close.h"
#include "edt_prefab_shape.h"
#include <snk/editor/edt_action_registry.h>
#include "edt_prefab_save_all.h"
#include "edt_save_all.h"
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_model.h>
#include <entt/entt.hpp>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
		// The document_manager gives ids; here nothing does, and id 0 means "no document".
		document->assign_id(7);
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

	edt::prefab_variant_creator variant_creator(std::optional<std::string> blocked = std::nullopt)
	{
		return edt::prefab_variant_creator(edt::document_creator::ports{
			[this] { return descs.get(); },
			[this] { return rs.get(); },
			[this] { return std::vector<fs::path>{ lower, res_root }; },
			[blocked] { return blocked; } });
	}

	bool rename_arm(const std::string& name)
	{
		return document->edit([&name](scn::prefab_desc& prefab) {
			prefab.get_root().children[0].name = name;
			return true;
		});
	}

	std::string arm_name() const { return document->working()->get_root().children[0].name; }

	static bool registry_has_node_named(entt::registry* reg, const std::string& name)
	{
		if (!reg) return false;
		for (auto&& [entity, component] : reg->view<scn::name_component>().each()) {
			if (component.name == name) return true;
		}
		return false;
	}

	bool preview_has_node_named(const std::string& name)
	{
		return registry_has_node_named(document->get_registry(), name);
	}

	// An edited prefab whose file changed on disk (the watch ran): Apply refuses it.
	void make_conflicted()
	{
		document->open(tag);
		BOOST_REQUIRE(document->is_open());
		BOOST_REQUIRE(rename_arm("arm2"));
		write(res_root, "prefabs/a.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000a1","name":"outside"}]})");
		rs->signal_changed(tag);
		pump();
		BOOST_REQUIRE(document->changed_on_disk());
	}

	// The question of Save All about `document`, answered with the dialog's choice `index` (-1: the dialog was not asked).
	// Returns what `done` said: 1 resolved, 0 not, -1 never called.
	int ask_and_answer(edt::save_all_question question, int index)
	{
		edt::dialog_service dialogs;
		dialogs.set_registrar([](std::string_view, std::function<bool()>) {});
		const auto alive = std::make_shared<int>(0);
		const edt::prefab_question_ports ports{ dialogs, [this] { return document.get(); }, alive };
		int outcome = -1;
		edt::ask_prefab_question(ports, *document, question, [&outcome](bool resolved) { outcome = resolved ? 1 : 0; });
		const auto shown = dialogs.list();
		BOOST_REQUIRE(shown.size() == 1u);
		BOOST_TEST(shown[0].kind == "choice");
		BOOST_TEST(outcome == -1);
		BOOST_REQUIRE(dialogs.answer(shown[0].id, boost::json::object{ { "index", index } }).ok);
		return outcome;
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

// The preview is built as content of the edited file from the start (#993): a record of the working copy that instantiates
// that file itself is refused after open as after an edit, as when the saved file is spawned.
BOOST_AUTO_TEST_CASE(Open_RefusesARecordThatInstantiatesTheEditedPrefab_AsAnEditDoes)
{
	write(res_root, "prefabs/leaf.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"name":"tip"}]})");
	const res::tag self = res::tag::make("prefabs/self.prefab.desc");
	write(res_root, "prefabs/self.prefab.desc", std::string(R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000c1","name":"n","components":{"ref":")") + std::string(res::tag::make("prefabs/leaf.prefab.desc").view())
		+ R"("},"overrides":[{"op":"add_node","target":[],"value":{"__id":"00000000000000e1","name":"added",
			"components":{"ref":")" + std::string(self.view()) + R"("}}}]}]})");

	// How many children the node `added` has in the preview; none when it is not there.
	const auto children_of_added = [this] () -> std::optional<std::size_t> {
		entt::registry* reg = document->get_registry();
		if (!reg) return std::nullopt;
		for (auto&& [entity, name] : reg->view<scn::name_component>().each()) {
			if (name.name != "added") continue;
			if (reg->all_of<scn::prefab_instance>(entity)) return std::nullopt;
			const auto* kids = reg->try_get<scn::children_component>(entity);
			return kids ? kids->children.size() : std::size_t{ 0 };
		}
		return std::nullopt;
	};

	document->open(self);
	BOOST_REQUIRE(document->is_open());
	BOOST_TEST(preview_has_node_named("tip"));
	BOOST_TEST((children_of_added() == std::optional<std::size_t>{ 0 }));

	BOOST_REQUIRE(document->edit([](scn::prefab_desc& prefab) {
		prefab.get_root().children[0].name = "n2";
		return true;
	}));
	BOOST_TEST(preview_has_node_named("n2"));
	BOOST_TEST((children_of_added() == std::optional<std::size_t>{ 0 }));
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
	BOOST_TEST(document->changed_on_disk());
	BOOST_TEST((edt::save_all_question_of(*document) == edt::save_all_question::disk_changed));
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

BOOST_AUTO_TEST_CASE(DisplayTitle_MarksUnappliedEdits)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_TEST(document->display_title() == document->title());
	BOOST_REQUIRE(rename_arm("arm2"));
	BOOST_TEST(document->display_title() == document->title() + " [*]");
	BOOST_TEST(document->save().saved);
	BOOST_TEST(document->display_title() == document->title());
}

BOOST_AUTO_TEST_CASE(HotReloadOfThePrefab_DoesNotWipeTheEdits)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));
	rs->signal_changed(tag);
	pump();
	hot->tick();
	BOOST_TEST(preview_has_node_named("arm2"));
	BOOST_TEST(arm_name() == "arm2");
	BOOST_TEST(document->is_dirty());
}

BOOST_AUTO_TEST_CASE(FileChangedOnDisk_WhileEdited_IsFlagged_AndReloadTakesIt)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));
	write(res_root, "prefabs/a.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000a1","name":"outside"}]})");
	rs->signal_changed(tag);
	pump();

	BOOST_TEST(document->changed_on_disk());
	BOOST_TEST(arm_name() == "arm2");
	BOOST_TEST(!document->save().saved);

	BOOST_TEST(document->reload().empty());
	BOOST_TEST(!document->changed_on_disk());
	BOOST_TEST(arm_name() == "outside");
	BOOST_TEST(!document->is_dirty());
}

BOOST_AUTO_TEST_CASE(FileChangedOnDisk_WhileEdited_OverwriteWritesTheEdits)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));
	write(res_root, "prefabs/a.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000a1","name":"outside"}]})");
	rs->signal_changed(tag);
	pump();
	BOOST_REQUIRE(document->changed_on_disk());

	const edt::save_result saved = document->overwrite();

	BOOST_TEST_REQUIRE(saved.saved, saved.error);
	BOOST_TEST(!document->changed_on_disk());
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc").find("arm2") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Overwrite_ThatFails_KeepsTheFileMarkedAsChanged_AndApplyStaysRefused)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));
	write(res_root, "prefabs/a.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000a1","name":"outside"}]})");
	rs->signal_changed(tag);
	pump();
	BOOST_REQUIRE(document->changed_on_disk());
	document->set_lock_source([] { return true; });

	BOOST_TEST(!document->overwrite().saved);
	document->set_lock_source([] { return false; });

	BOOST_TEST(document->changed_on_disk());
	const edt::save_result saved = document->save();
	BOOST_TEST(!saved.saved);
	BOOST_TEST(saved.error.find("changed on disk") != std::string::npos);
	BOOST_TEST(arm_name() == "arm2");
}

BOOST_AUTO_TEST_CASE(Overwrite_OfACleanDocument_TakesTheFileInsteadOfClaimingSuccessDoingNothing)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	write(res_root, "prefabs/a.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000a1","name":"outside"}]})");

	const edt::save_result done = document->overwrite();

	BOOST_TEST_REQUIRE(done.saved, done.error);
	BOOST_TEST(arm_name() == "outside");
	BOOST_TEST(!document->is_dirty());
}

BOOST_AUTO_TEST_CASE(FileChangedOnDisk_WhileClean_IsTakenWithoutAsking)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	write(res_root, "prefabs/a.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000a1","name":"outside"}]})");
	rs->signal_changed(tag);
	pump();

	BOOST_TEST(!document->changed_on_disk());
	BOOST_TEST(arm_name() == "outside");
	BOOST_TEST(preview_has_node_named("outside"));
}

BOOST_AUTO_TEST_CASE(ApplyBlocker_NamesWhyApplyIsOff)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_TEST(document->apply_blocker().empty());
	document->set_lock_source([] { return true; });
	BOOST_TEST(!document->apply_blocker().empty());
}

BOOST_AUTO_TEST_CASE(ApplyBlocker_ALowerLayerPrefab_IsReadOnly)
{
	write(lower, "prefabs/low.prefab.desc", PREFAB);
	document->open(res::tag::make("prefabs/low.prefab.desc"));
	BOOST_REQUIRE(document->is_open());
	BOOST_TEST(document->apply_blocker().find("lower layer") != std::string::npos);
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

BOOST_AUTO_TEST_CASE(Apply_OfAVariant_KeepsItsParent_AndTheEdit)
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
	BOOST_TEST(file.find("\"__layer\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Apply_OfAVariant_ThatChangesAValueTheBaseAlreadyOverrides_WritesAnAddedRecord_AndReadsBackTheNewValue)
{
	write(res_root, "prefabs/base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000a1","name":"arm","overrides":[{"target":["00000000000000a1"],"component":"stat","path":"/hp","value":5,"op":"set"}]}]})");
	write(res_root, "prefabs/v.prefab.desc", R"({"__type":"prefab_desc","__version":4,"__parent":"res://prefabs/base.prefab.desc","__layer":2})");
	document->open(res::tag::make("prefabs/v.prefab.desc"));
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(document->edit([](scn::prefab_desc& prefab) {
		auto& records = prefab.get_root().children[0].overrides;
		records[0].value = 7;
		return true;
	}));

	BOOST_TEST_REQUIRE(document->save().saved);

	const std::string file = text_of(res_root, "prefabs/v.prefab.desc");
	BOOST_TEST(file.find("__parent") != std::string::npos);
	BOOST_TEST(file.find("\"__layer\"") != std::string::npos);
	BOOST_TEST(file.find("7") != std::string::npos);
	document->revert();
	BOOST_REQUIRE(!document->working()->get_root().children[0].overrides.empty());
	BOOST_TEST(document->working()->get_root().children[0].overrides.back().value.as_int64() == 7);
}

BOOST_AUTO_TEST_CASE(Apply_OfAVariant_ThatTakesOutAnInheritedRecord_IsRefused_AndSaveWithoutBaseWritesItWhole)
{
	write(res_root, "prefabs/base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000a1","name":"arm","overrides":[{"target":["00000000000000a1"],"component":"stat","path":"/hp","value":5,"op":"set"}]}]})");
	const std::string variant = R"({"__type":"prefab_desc","__version":4,"__parent":"res://prefabs/base.prefab.desc","__layer":2})";
	write(res_root, "prefabs/v.prefab.desc", variant);
	document->open(res::tag::make("prefabs/v.prefab.desc"));
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(document->edit([](scn::prefab_desc& prefab) {
		prefab.get_root().children[0].overrides.clear();
		return true;
	}));

	const edt::save_result refused = document->save();
	BOOST_TEST(!refused.saved);
	BOOST_TEST(document->layer_refused());
	BOOST_TEST(document->is_dirty());
	BOOST_TEST(text_of(res_root, "prefabs/v.prefab.desc") == variant);

	BOOST_TEST_REQUIRE(document->save_without_base().saved);
	BOOST_TEST(!document->is_dirty());
	const std::string file = text_of(res_root, "prefabs/v.prefab.desc");
	BOOST_TEST(file.find("__parent") == std::string::npos);
	BOOST_TEST(file.find("\"arm\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Edit_OfTheRootTransform_MovesTheRootOfThePreview_AndRevertPutsItBack)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	entt::registry* reg = document->get_registry();
	BOOST_REQUIRE(reg);
	const auto root_position = [reg]() -> glm::vec3 {
		for (auto&& [entity, instance] : reg->view<scn::prefab_instance>().each()) {
			if (const auto* placed = reg->try_get<scn::local_transform>(entity)) return glm::vec3(placed->local[3]);
		}
		return { -1.f, -1.f, -1.f };
	};
	const auto root_is_at = [&root_position](float x, float y, float z) {
		const glm::vec3 at = root_position();
		return at.x == x && at.y == y && at.z == z;
	};
	BOOST_TEST(root_is_at(0.f, 0.f, 0.f));

	// A position without has_transform: the document is edited, so the preview follows it too.
	BOOST_REQUIRE(document->edit([](scn::prefab_desc& prefab) {
		prefab.get_root().position = { 4.f, 5.f, 6.f };
		return true;
	}));
	BOOST_TEST(root_is_at(4.f, 5.f, 6.f));

	BOOST_TEST_REQUIRE(document->revert().empty());
	BOOST_TEST(root_is_at(0.f, 0.f, 0.f));
	BOOST_TEST(!document->is_dirty());
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

BOOST_AUTO_TEST_CASE(NotifyEdited_AfterAnInPlaceChange_FollowsInThePreviewAndMarksDirty_NothingIsWritten)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	const std::string before = text_of(res_root, "prefabs/a.prefab.desc");
	const std::uint64_t generation = document->generation();
	scn::prefab_desc::prefab_node* arm = document->working_node({ 0xa1 });
	BOOST_REQUIRE(arm);
	arm->name = "arm3";

	BOOST_TEST_REQUIRE(document->notify_edited());

	BOOST_TEST(document->is_dirty());
	BOOST_TEST(document->generation() > generation);
	BOOST_TEST(preview_has_node_named("arm3"));
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc") == before);
}

BOOST_AUTO_TEST_CASE(WorkingNode_ReachesNothingWhileLocked_OrForAWrongPath)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_TEST(document->working_node({}) != nullptr);
	BOOST_TEST(document->working_node({ 0xdead }) == nullptr);
	document->set_lock_source([] { return true; });
	BOOST_TEST(document->working_node({}) == nullptr);
	BOOST_TEST(!document->notify_edited());
}

BOOST_AUTO_TEST_CASE(Apply_IsSeenByAnInstanceInALevel)
{
	entt::registry level;
	hot->attach_registry(level);
	auto prefab = rs->require_sync<scn::prefab_desc>(tag);
	BOOST_REQUIRE(prefab.is_ready());
	const entt::entity instance = level.create();
	scn::assemble_prefab(*assembler, level, instance, *prefab, "a", tag);
	BOOST_REQUIRE(registry_has_node_named(&level, "arm"));

	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));
	BOOST_REQUIRE(registry_has_node_named(&level, "arm"));
	BOOST_TEST_REQUIRE(document->save().saved);
	for (int frame = 0; frame < 20 && !registry_has_node_named(&level, "arm2"); ++frame) {
		pump();
		hot->tick();
	}

	BOOST_TEST(registry_has_node_named(&level, "arm2"));
	BOOST_TEST(!registry_has_node_named(&level, "arm"));
	hot->detach_registry(level);
}

BOOST_AUTO_TEST_CASE(Apply_WhenTheWriteFails_LeavesTheDocumentEditedAndSaysSo)
{
	rs->unregistrate_resolver(res::tag::default_protocol());
	rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ lower, res_root },
		res::mount_access::read_only, res::write_policy::allow_all());
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("arm2"));
	const std::string before = text_of(res_root, "prefabs/a.prefab.desc");

	const edt::save_result saved = document->save();

	BOOST_TEST(!saved.saved);
	BOOST_TEST(saved.error.find("could not be written") != std::string::npos);
	BOOST_TEST(document->is_dirty());
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc") == before);
}

BOOST_AUTO_TEST_CASE(InspectorEdit_OfAPrefabSelection_GoesToTheDocument_AndNeverCommitsTheLevel)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::scene_document level;
	const std::uint64_t level_generation = level.generation();
	edt::inspector_target target;
	target.select_prefab(document->id(), { 0xa1 }, document->working_replaced());
	scn::prefab_desc::prefab_node* node = document->working_node(target.path());
	BOOST_REQUIRE(node);
	node->name = "arm3";

	BOOST_TEST(edt::route_node_changed(target, document.get(), level));

	BOOST_TEST(document->is_dirty());
	BOOST_TEST(preview_has_node_named("arm3"));
	BOOST_TEST(!level.is_dirty());
	BOOST_TEST(level.generation() == level_generation);
}

BOOST_AUTO_TEST_CASE(InspectorEdit_OfALevelSelection_CommitsTheLevel)
{
	edt::scene_document level;
	const std::uint64_t level_generation = level.generation();
	edt::inspector_target target;

	BOOST_TEST(edt::route_node_changed(target, nullptr, level));

	BOOST_TEST(level.generation() == level_generation + 1);
}

BOOST_AUTO_TEST_CASE(InspectorEdit_OfAPrefabThatIsGone_GoesNowhere)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::scene_document level;
	const std::uint64_t level_generation = level.generation();
	edt::inspector_target target;
	target.select_prefab(document->id() + 1, {}, 0);

	BOOST_TEST(!edt::route_node_changed(target, document.get(), level));
	BOOST_TEST(!edt::route_node_changed(target, nullptr, level));

	BOOST_TEST(level.generation() == level_generation);
	BOOST_TEST(!document->is_dirty());
}

BOOST_AUTO_TEST_CASE(WorkingReplaced_MovesWithARevert_SoAStalePointerIsKnown)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	const std::uint64_t replaced = document->working_replaced();
	BOOST_REQUIRE(rename_arm("arm2"));
	BOOST_TEST(document->working_replaced() == replaced);

	BOOST_TEST_REQUIRE(document->revert().empty());

	BOOST_TEST(document->working_replaced() > replaced);
}

namespace {

struct close_probe
{
	edt::prefab_close_ports ports()
	{
		edt::prefab_close_ports out;
		out.ask = [this](std::function<void(edt::unsaved_choice)> answer) {
			++questions;
			asked = std::move(answer);
		};
		out.apply = [this] { ++applies; return apply_result; };
		out.report_failure = [this](const std::string& error) { failure = error; };
		return out;
	}
	int questions = 0;
	int applies = 0;
	int closed = 0;
	std::string failure;
	edt::save_result apply_result = edt::save_result::ok();
	std::function<void(edt::unsaved_choice)> asked;
};

}

BOOST_AUTO_TEST_CASE(CloseQuestion_ACleanDocumentClosesWithoutAsking)
{
	close_probe probe;
	edt::close_prefab_with_question(false, probe.ports(), [&probe] { ++probe.closed; });
	BOOST_TEST(probe.closed == 1);
	BOOST_TEST(probe.questions == 0);
}

BOOST_AUTO_TEST_CASE(CloseQuestion_SaveApplies_ThenCloses)
{
	close_probe probe;
	edt::close_prefab_with_question(true, probe.ports(), [&probe] { ++probe.closed; });
	BOOST_REQUIRE(probe.questions == 1);
	BOOST_TEST(probe.closed == 0);
	probe.asked(edt::unsaved_choice::save);
	BOOST_TEST(probe.applies == 1);
	BOOST_TEST(probe.closed == 1);
}

BOOST_AUTO_TEST_CASE(CloseQuestion_AFailedSave_KeepsTheDocumentOpenAndReports)
{
	close_probe probe;
	probe.apply_result = edt::save_result::failure("nope");
	edt::close_prefab_with_question(true, probe.ports(), [&probe] { ++probe.closed; });
	probe.asked(edt::unsaved_choice::save);
	BOOST_TEST(probe.closed == 0);
	BOOST_TEST(probe.failure == "nope");
}

BOOST_AUTO_TEST_CASE(CloseQuestion_DiscardCloses_CancelStays)
{
	close_probe discard;
	edt::close_prefab_with_question(true, discard.ports(), [&discard] { ++discard.closed; });
	discard.asked(edt::unsaved_choice::discard);
	BOOST_TEST(discard.closed == 1);
	BOOST_TEST(discard.applies == 0);

	close_probe cancel;
	edt::close_prefab_with_question(true, cancel.ports(), [&cancel] { ++cancel.closed; });
	cancel.asked(edt::unsaved_choice::cancel);
	BOOST_TEST(cancel.closed == 0);
	BOOST_TEST(cancel.applies == 0);
}

BOOST_AUTO_TEST_CASE(ToolbarState_FollowsTheDocument)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::prefab_toolbar_state state = edt::toolbar_state_of(*document);
	BOOST_TEST(!state.apply_enabled);
	BOOST_TEST(state.revert_enabled);

	BOOST_REQUIRE(rename_arm("arm2"));
	state = edt::toolbar_state_of(*document);
	BOOST_TEST(state.apply_enabled);
	BOOST_TEST(state.apply_hint.empty());

	write(res_root, "prefabs/a.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000a1","name":"outside"}]})");
	rs->signal_changed(tag);
	pump();
	state = edt::toolbar_state_of(*document);
	BOOST_TEST(!state.apply_enabled);
	BOOST_TEST(state.apply_hint.find("changed on disk") != std::string::npos);

	document->set_lock_source([] { return true; });
	state = edt::toolbar_state_of(*document);
	BOOST_TEST(!state.apply_enabled);
	BOOST_TEST(!state.revert_enabled);
	BOOST_TEST(state.apply_hint.find("Play") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Close_UnwatchesTheFile)
{
	const std::size_t before = rs->watched_tag_count();
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_TEST(rs->watched_tag_count() > before);
	document->close();
	BOOST_TEST(rs->watched_tag_count() == before);
}

BOOST_AUTO_TEST_CASE(AnOverrideOnADeletedNode_IsSkippedWithAWarning_WhenAnInstanceIsBuilt)
{
	write(res_root, "prefabs/user.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000b1","name":"Inst","components":{"inst":{"__type":"prefab_desc","__parent":"res://prefabs/a.prefab.desc"}},
		 "overrides":[{"target":["00000000000000ff"],"component":"stat","path":"/hp","value":9}]}]})");
	std::vector<std::string> warnings;
	const engine::log_sink_handle sink = engine::add_log_sink([&warnings](std::string_view, engine::log_level level, std::string_view text) {
		if (level == engine::log_level::warn) warnings.emplace_back(text);
	});
	const res::tag user_tag = res::tag::make("prefabs/user.prefab.desc");
	auto user = rs->require_sync<scn::prefab_desc>(user_tag);
	BOOST_REQUIRE(user.is_ready());
	entt::registry level;

	scn::assemble_prefab(*assembler, level, level.create(), *user, "user", user_tag);

	const bool warned = std::ranges::any_of(warnings, [](const std::string& text) { return text.find("override: no node") != std::string::npos; });
	BOOST_TEST(warned);
	BOOST_TEST(registry_has_node_named(&level, "Inst"));
}

BOOST_AUTO_TEST_CASE(Close_DropsTheWorkingCopy)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	document->close();
	BOOST_TEST(document->working() == nullptr);
	BOOST_TEST(!document->is_dirty());
}

BOOST_AUTO_TEST_CASE(ApplyOverrideRecords_MovesTheValueIntoTheFile_AndTellsWhatItDoes)
{
	const scn::override_record record{ .target = {}, .component = "stat", .path = "/hp", .value = 77 };

	const std::string error = edt::apply_override_records(services, layout(), tag, { record }, nullptr);

	BOOST_TEST(error.empty());
	const std::string file = text_of(res_root, "prefabs/a.prefab.desc");
	BOOST_TEST(file.find("77") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ApplyOverrideRecords_RefusesWhileThePrefabIsOpenWithUnappliedEdits_WritingNothing)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("renamed"));
	const std::string before = text_of(res_root, "prefabs/a.prefab.desc");
	const scn::override_record record{ .target = {}, .component = "stat", .path = "/hp", .value = 77 };

	const std::string error = edt::apply_override_records(services, layout(), tag, { record }, document.get());

	BOOST_TEST(error.find("apply or revert") != std::string::npos);
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc") == before);
}

BOOST_AUTO_TEST_CASE(ApplyOverrideRecords_ALowerLayerPrefab_IsNotWritten)
{
	write(lower, "prefabs/low.prefab.desc", PREFAB);
	const scn::override_record record{ .target = {}, .component = "stat", .path = "/hp", .value = 77 };

	const std::string error = edt::apply_override_records(services, layout(), res::tag::make("prefabs/low.prefab.desc"), { record }, nullptr);

	BOOST_TEST(!error.empty());
	BOOST_TEST(text_of(lower, "prefabs/low.prefab.desc") == PREFAB);
}

BOOST_AUTO_TEST_CASE(FoldRecord_BelowANestedInstance_BecomesARecordOfThatInstanceNode)
{
	write(res_root, "prefabs/user.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000b1","name":"Inst","components":{"inst":{"__type":"prefab_desc","__parent":"res://prefabs/a.prefab.desc"}}}]})");
	auto user = require_loaded<scn::prefab_desc>(res::tag::make("prefabs/user.prefab.desc"));
	pump();
	BOOST_REQUIRE(user.is_ready());
	scn::prefab_desc edited;
	user->copy_to(edited);
	const scn::override_record record{ .target = { 0xb1, 0xa1 }, .component = "stat", .path = "/hp", .value = 9 };

	const std::string error = edt::override_edit::fold_record(edited, record);

	BOOST_TEST(error.empty());
	const auto& inst = edited.get_root().children.at(0);
	BOOST_REQUIRE(inst.overrides.size() == 1);
	BOOST_TEST((inst.overrides[0].target == std::vector<uint64_t>{ 0xa1 }));
	BOOST_TEST(inst.overrides[0].path == "/hp");
}

BOOST_AUTO_TEST_CASE(FoldRecord_ANodeThePrefabLacks_AndRemoveRecords_AreRefusedLeavingThePrefabAlone)
{
	auto handle = rs->require_sync<scn::prefab_desc>(tag);
	BOOST_REQUIRE(handle.is_ready());
	scn::prefab_desc edited;
	handle->copy_to(edited);
	const std::string before = std::to_string(edited.get_root().children.size());

	const std::string missing = edt::override_edit::fold_record(edited,
		scn::override_record{ .target = { 0xff }, .component = "stat", .path = "/hp", .value = 1 });
	const std::string removal = edt::override_edit::fold_record(edited,
		scn::override_record{ .target = {}, .component = "stat", .path = {}, .value = {}, .op = scn::override_op::remove });

	BOOST_TEST(!missing.empty());
	BOOST_TEST(!removal.empty());
	BOOST_TEST(std::to_string(edited.get_root().children.size()) == before);
}

BOOST_AUTO_TEST_CASE(FoldRecord_ATransformRecord_ChangesThePlacementOfTheNode)
{
	auto handle = rs->require_sync<scn::prefab_desc>(tag);
	BOOST_REQUIRE(handle.is_ready());
	scn::prefab_desc edited;
	handle->copy_to(edited);

	const std::string error = edt::override_edit::fold_record(edited, scn::override_record{ .target = { 0xa1 },
		.component = std::string(scn::transform_pseudo_component), .path = "/position", .value = boost::json::array{ 1, 2, 3 } });

	BOOST_TEST(error.empty());
	const auto& arm = edited.get_root().children.at(0);
	BOOST_TEST(arm.position.y == 2.0f);
	BOOST_TEST(arm.has_transform);
}

BOOST_AUTO_TEST_CASE(TheApplyQuestion_NamesThePrefab_TheInstancesAndTheRecords)
{
	const std::string text = edt::apply_to_prefab_question(tag, 3, 2);

	BOOST_TEST(text.find("prefabs/a.prefab.desc") != std::string::npos);
	BOOST_TEST(text.find("for all of its instances (3 in this level") != std::string::npos);
	BOOST_TEST(text.find("cannot be undone") != std::string::npos);
	BOOST_TEST(text.find("2 of this instance") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ApplyOverrideRecords_IsAllOrNone_AFailingRecordLeavesTheFileAlone)
{
	const std::string before = text_of(res_root, "prefabs/a.prefab.desc");
	const scn::override_record fits{ .target = {}, .component = "stat", .path = "/hp", .value = 77 };
	const scn::override_record gone{ .target = { 0xff }, .component = "stat", .path = "/hp", .value = 1 };

	const std::string error = edt::apply_override_records(services, layout(), tag, { fits, gone }, nullptr);

	BOOST_TEST(!error.empty());
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc") == before);
}

BOOST_AUTO_TEST_CASE(FoldRecord_TheSameFieldTwice_IsOneRecordOfTheInstanceNodeWithTheLaterValue)
{
	write(res_root, "prefabs/user.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000b1","name":"Inst","components":{"inst":{"__type":"prefab_desc","__parent":"res://prefabs/a.prefab.desc"}}}]})");
	auto user = require_loaded<scn::prefab_desc>(res::tag::make("prefabs/user.prefab.desc"));
	pump();
	BOOST_REQUIRE(user.is_ready());
	scn::prefab_desc edited;
	user->copy_to(edited);

	BOOST_REQUIRE(edt::override_edit::fold_record(edited, scn::override_record{ .target = { 0xb1, 0xa1 }, .component = "stat", .path = "/hp", .value = 9 }).empty());
	BOOST_REQUIRE(edt::override_edit::fold_record(edited, scn::override_record{ .target = { 0xb1, 0xa1 }, .component = "stat", .path = "/hp", .value = 11 }).empty());

	const auto& inst = edited.get_root().children.at(0);
	BOOST_REQUIRE(inst.overrides.size() == 1u);
	BOOST_TEST(inst.overrides[0].value.to_number<int>() == 11);
}

BOOST_AUTO_TEST_CASE(CreateVariant_WritesOnlyTheParent_NextToTheBase)
{
	const auto made = variant_creator().create(res::tag::make("prefabs/a.prefab.desc"), "");

	BOOST_REQUIRE_MESSAGE(made.ok(), made.error);
	BOOST_TEST(made.tag.view() == "res://prefabs/a_variant.prefab.desc");
	const std::string file = text_of(res_root, "prefabs/a_variant.prefab.desc");
	BOOST_TEST(file.find("__parent") != std::string::npos);
	BOOST_TEST(file.find("res://prefabs/a.prefab.desc") != std::string::npos);
	BOOST_TEST(file.find("prefab_desc") != std::string::npos);
	BOOST_TEST(file.find("children") == std::string::npos);
	BOOST_TEST(file.find("\"__layer\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(CreateVariant_OfABaseThatDoesNotLoad_IsRefusedWritingNothing)
{
	const auto made = variant_creator().create(res::tag::make("prefabs/missing.prefab.desc"), "x");

	BOOST_TEST(!made.ok());
	BOOST_TEST(made.error.find("does not load as a prefab") != std::string::npos);
	BOOST_TEST(!fs::exists(res_root / "prefabs" / "x.prefab.desc"));
}

BOOST_AUTO_TEST_CASE(CreateVariant_ANameTheClaimRefuses_WritesNothing)
{
	const edt::prefab_variant_creator creator(
		edt::document_creator::ports{
			[this] { return descs.get(); }, [this] { return rs.get(); },
			[this] { return std::vector<fs::path>{ lower, res_root }; }, [] { return std::optional<std::string>{}; } },
		[](const res::tag&) { return std::string{ "a name of the open level" }; });

	const auto made = creator.create(res::tag::make("prefabs/a.prefab.desc"), "x");

	BOOST_TEST(made.error == "a name of the open level");
	BOOST_TEST(!fs::exists(res_root / "prefabs" / "x.prefab.desc"));
}

BOOST_AUTO_TEST_CASE(OpenLevelClaim_NamesTheOpenLevelsFile_AndNothingElse)
{
	edt::desc_file_layout level_layout = layout();
	level_layout.open_level_files = [] { return std::vector<std::string>{ "levels/lv.desc" }; };

	BOOST_TEST(!edt::open_level_claim(*rs, *descs, level_layout, res::tag::make("levels/lv.desc")).empty());
	BOOST_TEST(edt::open_level_claim(*rs, *descs, level_layout, res::tag::make("prefabs/other.prefab.desc")).empty());
}

BOOST_AUTO_TEST_CASE(CreateVariant_ATakenName_GetsANumber_AndANamedOneThatIsTakenIsRefused)
{
	const auto creator = variant_creator();
	BOOST_REQUIRE(creator.create(res::tag::make("prefabs/a.prefab.desc"), "").ok());

	BOOST_TEST(creator.suggested_name(res::tag::make("prefabs/a.prefab.desc")) == "a_variant2");
	const auto again = creator.create(res::tag::make("prefabs/a.prefab.desc"), "a_variant");
	BOOST_TEST(!again.ok());
	BOOST_TEST(again.error.find("already exists") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(CreateVariant_OfSomethingThatIsNoPrefab_OrWhileTheProjectIsLocked_IsRefusedWritingNothing)
{
	BOOST_TEST(!variant_creator().create(res::tag::make("prefabs/a.desc"), "x").ok());
	const auto locked = variant_creator(std::string{ "a migration runs" }).create(res::tag::make("prefabs/a.prefab.desc"), "x");
	BOOST_TEST(locked.error == "a migration runs");
	BOOST_TEST(!fs::exists(res_root / "prefabs" / "x.prefab.desc"));
}

BOOST_AUTO_TEST_CASE(AVariantOfAVariantOfAVariant_LoadsWithTheContentOfTheBase)
{
	const auto creator = variant_creator();
	const auto second = creator.create(res::tag::make("prefabs/a.prefab.desc"), "b");
	BOOST_REQUIRE_MESSAGE(second.ok(), second.error);
	const auto third = creator.create(second.tag, "c");
	BOOST_REQUIRE_MESSAGE(third.ok(), third.error);
	const auto fourth = creator.create(third.tag, "d");
	BOOST_REQUIRE_MESSAGE(fourth.ok(), fourth.error);

	auto last = require_loaded<scn::prefab_desc>(fourth.tag);
	pump();

	BOOST_REQUIRE(last.is_ready());
	const auto& root = last->get_root();
	BOOST_REQUIRE(root.children.size() == 1u);
	BOOST_TEST(root.children[0].name == "arm");
}

BOOST_AUTO_TEST_CASE(AVariantThatNamesItselfAsItsParent_ReadsAsTheOwnerWantsIt_InProblems)
{
	write(res_root, "prefabs/loop.prefab.desc", R"({"__type":"prefab_desc","__version":4,"__parent":"res://prefabs/loop.prefab.desc"})");
	const res::tag loop = res::tag::make("prefabs/loop.prefab.desc");
	auto handle = rs->require_sync<scn::prefab_desc>(loop);
	pump();

	const auto items = edt::diagnostics_of(handle);

	BOOST_REQUIRE(!items.empty());
	const auto cycle = std::ranges::find(items, std::string(res::diagnostic_kind::cycle), &res::diagnostic::kind);
	BOOST_REQUIRE(cycle != items.end());
	const edt::problem row = edt::desc_problem_publisher::to_problem(*cycle);
	BOOST_TEST(row.message.find("Prefab 'res://prefabs/loop.prefab.desc' refers to itself through") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ApplyingAVariant_WritesOnlyItsOwnLayer_NotTheBasesContent)
{
	const auto made = variant_creator().create(res::tag::make("prefabs/a.prefab.desc"), "v");
	BOOST_REQUIRE_MESSAGE(made.ok(), made.error);
	document->open(made.tag);
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(rename_arm("renamed"));

	const auto saved = document->save();

	BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
	const std::string file = text_of(res_root, "prefabs/v.prefab.desc");
	BOOST_TEST(file.find("__parent") != std::string::npos);
	BOOST_TEST(file.find("renamed") != std::string::npos);
	BOOST_TEST(file.find("\"hp\"") == std::string::npos, "the variant file holds: " << file);
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc").find("renamed") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheVariantForm_StartsWithTheSuggestedName_AndSubmitCreatesIt)
{
	const auto creator = variant_creator();
	edt::prefab_variant_form form(creator);

	form.open(res::tag::make("prefabs/a.prefab.desc"));

	BOOST_TEST(form.name() == "a_variant");
	BOOST_TEST(form.can_submit());
	const auto made = form.submit();
	BOOST_REQUIRE(made.has_value());
	BOOST_TEST(made->view() == "res://prefabs/a_variant.prefab.desc");
	BOOST_TEST(fs::exists(res_root / "prefabs" / "a_variant.prefab.desc"));
}

BOOST_AUTO_TEST_CASE(TheVariantForm_ATakenName_ShowsWhyAndWritesNothingOverTheFile)
{
	write(res_root, "prefabs/taken.prefab.desc", "kept");
	const auto creator = variant_creator();
	edt::prefab_variant_form form(creator);
	form.open(res::tag::make("prefabs/a.prefab.desc"));

	form.set_name("taken");

	BOOST_TEST(!form.can_submit());
	BOOST_TEST(form.problem().find("already exists") != std::string::npos);
	BOOST_TEST(!form.submit().has_value());
	BOOST_TEST(text_of(res_root, "prefabs/taken.prefab.desc") == "kept");
}

// #746 S3: the Add shape menu of the prefab editor puts a node with a mesh under the picked node of the working copy.

namespace {

const boost::json::object& mesh_fields_of(const scn::prefab_desc::prefab_node& node)
{
	return node.components.at("mesh").overrides;
}

}

BOOST_AUTO_TEST_CASE(AddShape_OfABaseShape_PutsAMeshNodeUnderTheParent_AndMakesTheDocumentDirty)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::action_registry actions;
	const edt::mesh::base_shape& shape = edt::mesh::base_shapes().front();

	const std::string error = edt::add_shape_to_prefab(actions, *document, { std::string(shape.name), {}, { 0xa1 } });

	BOOST_TEST(error == "");
	BOOST_TEST(document->is_dirty());
	const auto& arm = document->working()->get_root().children[0];
	BOOST_REQUIRE(arm.children.size() == 1);
	BOOST_TEST(arm.children[0].name == std::string(shape.name));
	BOOST_TEST(arm.children[0].node_id != 0u);
	BOOST_TEST(arm.children[0].components.at("mesh").type_name == "mesh_node_desc");
	BOOST_TEST(mesh_fields_of(arm.children[0]).at("geometry").as_string() == edt::mesh::base_shape_path(shape));
}

BOOST_AUTO_TEST_CASE(AddShape_WithAnEmptyParentPath_GoesUnderTheRoot)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::action_registry actions;
	const std::size_t before = document->working()->get_root().children.size();

	BOOST_TEST(edt::add_shape_to_prefab(actions, *document, { std::string(edt::mesh::base_shapes().front().name), {}, {} }) == "");

	BOOST_TEST(document->working()->get_root().children.size() == before + 1);
}

BOOST_AUTO_TEST_CASE(AddShape_UnderANodeThatIsGone_OrAnUnknownShape_ChangesNothing)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::action_registry actions;

	BOOST_TEST(edt::add_shape_to_prefab(actions, *document, { std::string(edt::mesh::base_shapes().front().name), {}, { 0xdead } }) != "");
	BOOST_TEST(edt::add_shape_to_prefab(actions, *document, { "no_such_shape", {}, {} }) != "");
	BOOST_TEST(edt::add_shape_to_prefab(actions, *document, { {}, {}, {} }) != "");
	BOOST_TEST(!document->is_dirty());
}

BOOST_AUTO_TEST_CASE(AddShape_OfAParametricShape_TakesTheRangeFromWhatShapeCreateAnswered)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::action_registry actions;
	boost::json::object asked;
	const std::string geometry_tag = std::string("res://") + "shapes/ball.geom.desc";
	cmd::command_desc create;
	create.id = "shape.create";
	create.summary = "stub";
	create.args_schema = boost::json::parse(R"({"type":"object"})");
	create.result_schema = boost::json::parse(R"({"type":"object"})");
	create.mutates = true;
	create.category = cmd::agent_category::scene_edit;
	create.invoke = [&asked, geometry_tag](const cmd::call& call) {
		asked = call.args.as_object();
		return cmd::result::success(boost::json::object{ { "poly", "shapes/ball.poly.desc" }, { "geometry", geometry_tag }, { "vx_begin", 0 },
			{ "vx_end", 50 }, { "ind_begin", 0 }, { "ind_end", 240 } });
	};
	const edt::registration made = actions.commands().add(std::move(create));

	const std::string error = edt::add_shape_to_prefab(actions, *document, { {}, boost::json::object{ { "name", "ball" }, { "kind", "sphere" } }, { 0xa1 } });

	BOOST_TEST(error == "");
	BOOST_TEST(asked.at("name").as_string() == "ball");
	const auto& node = document->working()->get_root().children[0].children.at(0);
	BOOST_TEST(node.name == "ball");
	BOOST_TEST(mesh_fields_of(node).at("geometry").as_string() == geometry_tag);
	BOOST_TEST(mesh_fields_of(node).at("ind_end").to_number<int>() == 240);
}

BOOST_AUTO_TEST_CASE(AddShape_OfAParametricShape_ThatShapeCreateRefuses_ChangesNothing)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::action_registry actions;
	cmd::command_desc create;
	create.id = "shape.create";
	create.summary = "stub";
	create.args_schema = boost::json::parse(R"({"type":"object"})");
	create.result_schema = boost::json::parse(R"({"type":"object"})");
	create.mutates = true;
	create.category = cmd::agent_category::scene_edit;
	create.invoke = [](const cmd::call&) { return cmd::result::failure(cmd::codes::needs_input, "shapes/ball.poly.desc exists"); };
	const edt::registration made = actions.commands().add(std::move(create));

	const std::string error = edt::add_shape_to_prefab(actions, *document, { {}, boost::json::object{ { "name", "ball" }, { "kind", "sphere" } }, {} });

	BOOST_TEST(error.find("shape.create") != std::string::npos);
	BOOST_TEST(error.find("exists") != std::string::npos);
	BOOST_TEST(!document->is_dirty());
}

BOOST_AUTO_TEST_CASE(AddShape_OfAParametricShape_UnderANodeThatIsGone_AsksNothingOfShapeCreate)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::action_registry actions;
	int creates = 0;
	cmd::command_desc create;
	create.id = "shape.create";
	create.summary = "stub";
	create.args_schema = boost::json::parse(R"({"type":"object"})");
	create.result_schema = boost::json::parse(R"({"type":"object"})");
	create.mutates = true;
	create.category = cmd::agent_category::scene_edit;
	create.invoke = [&creates](const cmd::call&) {
		++creates;
		return cmd::result::success();
	};
	const edt::registration made = actions.commands().add(std::move(create));

	const std::string error = edt::add_shape_to_prefab(actions, *document, { {}, boost::json::object{ { "name", "ball" }, { "kind", "sphere" } }, { 0xdead } });

	BOOST_TEST(error != "");
	BOOST_TEST(creates == 0);
	BOOST_TEST(!document->is_dirty());
}

BOOST_AUTO_TEST_CASE(SaveAll_ChangedOnDiskPrefab_AsksOverwriteOrReload)
{
	make_conflicted();
	BOOST_TEST(!document->save().saved);
	BOOST_TEST((edt::save_all_question_of(*document) == edt::save_all_question::disk_changed));

	BOOST_TEST(ask_and_answer(edt::save_all_question::disk_changed, 1) == 1);

	BOOST_TEST(!document->changed_on_disk());
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc").find("arm2") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(SaveAll_ChangedOnDiskPrefab_ReloadTakesTheFile_CancelChangesNothing)
{
	make_conflicted();
	const std::string outside = text_of(res_root, "prefabs/a.prefab.desc");

	BOOST_TEST(ask_and_answer(edt::save_all_question::disk_changed, 2) == 0);
	BOOST_TEST(document->changed_on_disk());
	BOOST_TEST(document->is_dirty());
	BOOST_TEST(arm_name() == "arm2");
	BOOST_TEST(text_of(res_root, "prefabs/a.prefab.desc") == outside);

	BOOST_TEST(ask_and_answer(edt::save_all_question::disk_changed, 0) == 1);
	BOOST_TEST(!document->changed_on_disk());
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(arm_name() == "outside");
}

BOOST_AUTO_TEST_CASE(SaveAll_AVariantThatNeedsItsBaseDropped_IsAskedAndSavesWholeOnlyOnAYes)
{
	write(res_root, "prefabs/base.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000a1","name":"arm","overrides":[{"target":["00000000000000a1"],"component":"stat","path":"/hp","value":5,"op":"set"}]}]})");
	const std::string variant = R"({"__type":"prefab_desc","__version":4,"__parent":"res://prefabs/base.prefab.desc","__layer":2})";
	write(res_root, "prefabs/v.prefab.desc", variant);
	document->open(res::tag::make("prefabs/v.prefab.desc"));
	BOOST_REQUIRE(document->is_open());
	BOOST_REQUIRE(document->edit([](scn::prefab_desc& prefab) {
		prefab.get_root().children[0].overrides.clear();
		return true;
	}));
	BOOST_REQUIRE(!document->save().saved);
	BOOST_TEST((edt::save_all_question_of(*document) == edt::save_all_question::base_layer));

	BOOST_TEST(ask_and_answer(edt::save_all_question::base_layer, 1) == 0);
	BOOST_TEST(text_of(res_root, "prefabs/v.prefab.desc") == variant);
	BOOST_TEST(document->is_dirty());

	BOOST_TEST(ask_and_answer(edt::save_all_question::base_layer, 0) == 1);
	BOOST_TEST(!document->is_dirty());
	BOOST_TEST(text_of(res_root, "prefabs/v.prefab.desc").find("__parent") == std::string::npos);
}

// #746: a prefab with a shape survives Apply and a fresh read: the mesh node keeps its geometry file and its range.

BOOST_AUTO_TEST_CASE(AddShape_ThenApply_WritesTheMeshNode_AndARevertReadsItBack)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::action_registry actions;
	const edt::mesh::base_shape& shape = edt::mesh::base_shapes().front();
	const edt::mesh::material_range range = edt::mesh::base_shape_range(shape);
	BOOST_REQUIRE(edt::add_shape_to_prefab(actions, *document, { std::string(shape.name), {}, { 0xa1 } }) == "");

	const edt::save_result saved = document->save();
	BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);

	const std::string written = text_of(res_root, "prefabs/a.prefab.desc");
	BOOST_TEST(written.find(edt::mesh::base_shape_path(shape)) != std::string::npos);
	BOOST_TEST(written.find("mesh_node_desc") != std::string::npos);

	BOOST_REQUIRE(document->revert() == "");
	BOOST_TEST(!document->is_dirty());
	const auto& arm = document->working()->get_root().children.at(0);
	BOOST_REQUIRE(arm.children.size() == 1);
	BOOST_TEST(arm.children[0].name == std::string(shape.name));
	const boost::json::object& fields = mesh_fields_of(arm.children[0]);
	BOOST_TEST(fields.at("geometry").as_string() == edt::mesh::base_shape_path(shape));
	BOOST_TEST(fields.at("vx_begin").to_number<std::uint64_t>() == range.vx_begin);
	BOOST_TEST(fields.at("vx_end").to_number<std::uint64_t>() == range.vx_end);
	BOOST_TEST(fields.at("ind_begin").to_number<std::uint64_t>() == range.ind_begin);
	BOOST_TEST(fields.at("ind_end").to_number<std::uint64_t>() == range.ind_end);
}

// #746 S5: Rebuild from the Inspector rebuilds the shape (the command), then the open prefab's working copy follows or says why not.

namespace {

struct rebuild_stub
{
	boost::json::object answer;
	cmd::result refusal = cmd::result::success();
	bool refuses = false;
	int calls = 0;
	edt::registration registration;

	void install(edt::action_registry& actions)
	{
		cmd::command_desc rebuild;
		rebuild.id = "shape.rebuild";
		rebuild.summary = "stub";
		rebuild.args_schema = boost::json::parse(R"({"type":"object"})");
		rebuild.result_schema = boost::json::parse(R"({"type":"object"})");
		rebuild.mutates = true;
		rebuild.category = cmd::agent_category::scene_edit;
		rebuild.invoke = [this](const cmd::call&) {
			++calls;
			return refuses ? refusal : cmd::result::success(answer);
		};
		registration = actions.commands().add(std::move(rebuild));
	}
};

boost::json::object range_answer(int vx_end, int ind_end)
{
	return boost::json::object{ { "vx_begin", 0 }, { "vx_end", vx_end }, { "ind_begin", 0 }, { "ind_end", ind_end } };
}

}

BOOST_AUTO_TEST_CASE(RebuildShape_RetargetsTheNodesOfTheOpenPrefab_AndSaysNothingMore)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::action_registry actions;
	rebuild_stub stub;
	stub.answer = range_answer(500, 900);
	stub.install(actions);
	const edt::mesh::base_shape& shape = edt::mesh::base_shapes().front();
	BOOST_REQUIRE(edt::add_shape_to_prefab(actions, *document, { std::string(shape.name), {}, { 0xa1 } }) == "");
	const std::string geometry = edt::mesh::base_shape_path(shape);

	const edt::rebuild_report report = edt::rebuild_shape(actions, document.get(), boost::json::object{ { "path", "x" } }, geometry);

	BOOST_TEST(report.error == "");
	BOOST_TEST(report.notes.empty());
	BOOST_TEST(mesh_fields_of(document->working()->get_root().children[0].children.at(0)).at("vx_end").to_number<int>() == 500);
}

BOOST_AUTO_TEST_CASE(RebuildShape_ThatTheCommandRefuses_ReportsItAndTouchesNoNode_AlsoTheStalePath)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::action_registry actions;
	rebuild_stub stub;
	stub.refuses = true;
	stub.refusal = cmd::result::failure(cmd::codes::stale, "the file changed since it was read");
	stub.install(actions);
	const edt::mesh::base_shape& shape = edt::mesh::base_shapes().front();
	BOOST_REQUIRE(edt::add_shape_to_prefab(actions, *document, { std::string(shape.name), {}, { 0xa1 } }) == "");
	const std::string geometry = edt::mesh::base_shape_path(shape);
	const boost::json::object before = mesh_fields_of(document->working()->get_root().children[0].children.at(0));

	const edt::rebuild_report report = edt::rebuild_shape(actions, document.get(), boost::json::object{}, geometry);

	BOOST_TEST(report.error.find("stale") != std::string::npos);
	BOOST_TEST(report.error.find("the file changed since it was read") != std::string::npos);
	BOOST_TEST((mesh_fields_of(document->working()->get_root().children[0].children.at(0)) == before));
}

BOOST_AUTO_TEST_CASE(RebuildShape_WhenTheLevelCouldNotFollow_SaysSo)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::action_registry actions;
	rebuild_stub stub;
	stub.answer = range_answer(500, 900);
	stub.answer["scene_update"] = "the scene is playing: stop play first";
	stub.install(actions);

	const edt::rebuild_report report = edt::rebuild_shape(actions, document.get(), boost::json::object{}, "shapes/none.geom.desc");

	BOOST_TEST(report.error == "");
	BOOST_REQUIRE(report.notes.size() == 1u);
	BOOST_TEST(report.notes[0].find("the scene is playing") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(RebuildShape_InPlay_LeavesTheOpenPrefabAsItIs_AndSaysWhy)
{
	document->open(tag);
	BOOST_REQUIRE(document->is_open());
	edt::action_registry actions;
	rebuild_stub stub;
	stub.answer = range_answer(500, 900);
	stub.install(actions);
	const edt::mesh::base_shape& shape = edt::mesh::base_shapes().front();
	BOOST_REQUIRE(edt::add_shape_to_prefab(actions, *document, { std::string(shape.name), {}, { 0xa1 } }) == "");
	document->set_lock_source([] { return true; });
	const std::string geometry = edt::mesh::base_shape_path(shape);
	const boost::json::object before = mesh_fields_of(document->working()->get_root().children[0].children.at(0));

	const edt::rebuild_report report = edt::rebuild_shape(actions, document.get(), boost::json::object{}, geometry);

	BOOST_REQUIRE(report.notes.size() == 1u);
	BOOST_TEST(report.notes[0].find("Play is running") != std::string::npos);
	BOOST_TEST((mesh_fields_of(document->working()->get_root().children[0].children.at(0)) == before));
	BOOST_TEST(!edt::prefab_rebuild_blocker(*document).empty());
}

BOOST_AUTO_TEST_CASE(RebuildShape_ForAPrefabOfALowerLayer_LeavesItAsItIsAndSaysWhy)
{
	write(lower, "prefabs/low.prefab.desc", PREFAB);
	fs::remove(res_root / "prefabs" / "a.prefab.desc");
	document->open(res::tag::make("prefabs/low.prefab.desc"));
	BOOST_REQUIRE(document->is_open());
	edt::action_registry actions;
	rebuild_stub stub;
	stub.answer = range_answer(500, 900);
	stub.install(actions);
	const edt::mesh::base_shape& shape = edt::mesh::base_shapes().front();
	const std::string geometry = edt::mesh::base_shape_path(shape);
	BOOST_REQUIRE(document->edit([&](scn::prefab_desc& working) {
		working.get_root().children[0].children.push_back(edt::mesh::base_shape_node(shape));
		return true;
	}));
	const boost::json::object before = mesh_fields_of(document->working()->get_root().children[0].children.at(0));

	const edt::rebuild_report report = edt::rebuild_shape(actions, document.get(), boost::json::object{}, geometry);

	BOOST_REQUIRE(report.notes.size() == 1u);
	BOOST_TEST(report.notes[0].find("lower") != std::string::npos);
	BOOST_TEST((mesh_fields_of(document->working()->get_root().children[0].children.at(0)) == before));
}

BOOST_AUTO_TEST_SUITE_END()
