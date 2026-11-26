#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_commands.h"
#include "edt_asset_component.h"
#include "edt_asset_database.h"
#include "edt_asset_history.h"
#include "edt_asset_operation_form.h"
#include "edt_asset_ops.h"
#include "edt_asset_problems.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/project/prj_manifest.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_pretty_json.h>
#include <boost/json.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The asset browser's Rename, Move and Delete windows without ImGui (#775, EPIC-57 section 6.4, PR 3): the preview built from
// asset_ops::plan, the refusal texts, the second question of a forced delete, the report, the undo from it, the manifest copy
// read again and the Problems rows of what a forced delete left broken.
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

constexpr std::string_view k_sky_guid = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";
constexpr std::string_view k_other_guid = "5b1d2e3f-4a5b-4c6d-8e7f-0a1b2c3d4e5f";
constexpr std::string_view k_lonely_guid = "6c2e3f40-5b6c-4d7e-8f80-1b2c3d4e5f60";
constexpr std::string_view k_popular_guid = "7d3f4051-6c7d-4e8f-9091-2c3d4e5f6071";
constexpr std::string_view k_manifest_file = "project.snkproj";

std::string meta_text(std::string_view guid)
{
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(guid);
	meta.importer = "test";
	return res::write_asset_meta(meta);
}

std::string canonical(std::string_view text)
{
	const std::vector<std::byte> bytes = res::pretty_json_bytes(json::parse(text));
	return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

const char* const k_level = R"({
	"__type": "level_desc",
	"worlds": [ { "__type": "world_desc", "name": "w", "children": [ { "__id": "00000000000000a1",
		"components": {
			"sky": "res://sky.desc",
			"mesh": { "__type": "mesh_node_desc", "material": { "guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f", "path": "res://old_sky.desc" } },
			"inst": { "__type": "prefab_desc", "__parent": "res://sky.desc" },
			"other": { "__type": "mesh_node_desc", "material": "res://other.desc" }
		} } ] } ]
})";

const char* const k_manifest = R"({ "format_version": 1, "name": "Form", "engine_version": "0.2.0", "start_scene": "res://sky.desc" })";

bool contains(const std::vector<std::string>& lines, std::string_view needle)
{
	for (const std::string& line : lines) {
		if (line.find(needle) != std::string::npos) return true;
	}
	return false;
}

bool contains(const std::string& text, std::string_view needle)
{
	return text.find(needle) != std::string::npos;
}

struct form_fixture
{
	form_fixture() : layer("asset_form"), engine("asset_form_engine"), journal("asset_form_journal"), manifest_dir("asset_form_manifest")
	{
		write("sky.desc", canonical(R"({ "__type": "material_desc" })"));
		write("sky.desc.resource", meta_text(k_sky_guid));
		write("other.desc", canonical(R"({ "__type": "material_desc" })"));
		write("other.desc.resource", meta_text(k_other_guid));
		write("lonely.desc", canonical(R"({ "__type": "material_desc" })"));
		write("lonely.desc.resource", meta_text(k_lonely_guid));
		write("env/.keep", "x");
		write("levels/main.desc", canonical(k_level));
		write("materials/child.desc", canonical(R"({ "__type": "material_desc", "__parent": "res://sky.desc" })"));
		const fs::path manifest_path = manifest_dir.write(std::string(k_manifest_file), canonical(k_manifest));
		manifest_file = manifest_path;
		manifest.start_scene = "res://sky.desc";

		sources.operations = [this]() -> const edt::asset_ops* { return &operations; };
		sources.write_blocker = [this] { return blocked; };
		sources.done = [this](const edt::operation_report& report) {
			history.record(report.journal);
			edt::refresh_manifest_references(manifest_file, manifest);
		};
	}

	void write(std::string_view relative, std::string_view text)
	{
		const fs::path path = layer.write(relative, text);
		fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(1));
	}

	edt::asset_database& open()
	{
		db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ engine.path(), layer.path() });
		db->rebuild();
		test_util::pump(tasks);
		return *db;
	}

	void settle() { test_util::pump(tasks); }

	edt::asset_ops make_operations()
	{
		edt::asset_ops_ports ports;
		ports.assets = [this]() -> const edt::asset_database* { return db.get(); };
		ports.manifest = [this]() -> std::optional<edt::asset_ops_manifest> {
			edt::asset_ops_manifest out;
			out.root = manifest_dir.path();
			out.file = std::string(k_manifest_file);
			if (!manifest.start_scene.empty()) out.references.push_back(edt::manifest_reference{ "/start_scene", manifest.start_scene });
			return out;
		};
		ports.fail_step = [this](std::string_view step) -> std::string { return !fail_prefix.empty() && step.starts_with(fail_prefix) ? "boom" : ""; };
		ports.engine_base = engine.path();
		ports.journal_root = journal.path();
		ports.is_open_with_unsaved_changes = [this](const res::tag& tag) { return open_files.contains(std::string(tag.view())); };
		ports.hold_changes = []() -> std::shared_ptr<void> { return std::make_shared<int>(0); };
		ports.report_self_change = [this](std::span<const res::file_change> changes) {
			if (db) db->report_self_change(changes);
		};
		return edt::asset_ops(std::move(ports));
	}

	edt::asset_operation_ports make_ports()
	{
		edt::asset_operation_ports out;
		out.plan = [this](const edt::asset_request& request) { ++plan_calls; return operations.plan(std::span<const edt::asset_request>(&request, 1)); };
		out.run = [this](const edt::asset_request& request, bool force) { return edt::run_asset_operation(sources, request, force); };
		// What asset_component::undo_operation does: the newest operation only.
		out.undo = [this](const std::string& id) -> edt::asset_fix_result {
			if (history.next_operation_id() != std::optional<std::string>(id)) return edt::asset_fix_result{ false, "something was done after it", true };
			const edt::asset_fix_result result = history.undo_last(operations);
			if (result.ok) edt::refresh_manifest_references(manifest_file, manifest);
			return result;
		};
		return out;
	}

	std::string file_text(std::string_view relative) const
	{
		std::ifstream in(layer.path() / std::string(relative), std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	bool has(std::string_view relative) const { return fs::is_regular_file(layer.path() / std::string(relative)); }

	std::string manifest_text() const
	{
		std::ifstream in(manifest_file, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	std::map<std::string, std::string> snapshot() const
	{
		std::map<std::string, std::string> out;
		for (const fs::directory_entry& entry : fs::recursive_directory_iterator(layer.path())) {
			if (!entry.is_regular_file()) continue;
			const std::string relative = entry.path().lexically_relative(layer.path()).generic_string();
			out[relative] = file_text(relative);
		}
		return out;
	}

	std::vector<edt::problem> broken_rows()
	{
		std::vector<edt::problem> out;
		for (const edt::sourced_problem& row : problems.list()) {
			if (row.source == edt::asset_problem_publisher::source && row.item.code == "asset.broken_reference") out.push_back(row.item);
		}
		return out;
	}

	test_util::temp_dir layer;
	test_util::temp_dir engine;
	test_util::temp_dir journal;
	test_util::temp_dir manifest_dir;
	fs::path manifest_file;
	prj::manifest manifest;
	ds::task_system tasks{ test_util::manual_tasks() };
	edt::editor_events events;
	edt::job_service jobs{ tasks, &events };
	std::unique_ptr<edt::asset_database> db;
	std::set<std::string> open_files;
	std::optional<std::string> blocked;
	std::string fail_prefix;
	int plan_calls = 0;
	edt::asset_ops operations{ make_operations() };
	edt::asset_history history{ journal.path(), fs::path{}, [this] { return db->next_history_serial(); } };
	edt::asset_command_sources sources;
	edt::problem_list problems;
	edt::asset_problem_publisher publisher;
	edt::asset_operation_form form{ make_ports() };
};

}

BOOST_FIXTURE_TEST_SUITE(EdtAssetOperationFormTests, form_fixture)

BOOST_AUTO_TEST_CASE(ARenamePreview_ShowsTheTarget_TheFilesThatMove_AndWhatIsRewrittenPerFile)
{
	open();
	form.open(edt::asset_operation::rename, res::tag::make("sky.desc"));
	BOOST_TEST(form.input() == "sky.desc");
	BOOST_TEST(!form.can_submit(), "the name is the same as now");
	BOOST_REQUIRE(form.refused());
	BOOST_TEST((form.refused()->code == edt::refusal_code::no_change));

	form.set_input("clouds.desc");
	BOOST_REQUIRE(!form.refused());
	BOOST_REQUIRE(form.item());
	BOOST_TEST(form.can_submit());
	BOOST_TEST(!form.needs_force());
	const std::vector<std::string> lines = form.preview_lines();
	BOOST_TEST(contains(lines, "Rename res://sky.desc  ->  res://clouds.desc"));
	BOOST_TEST(contains(lines, "2 file(s) move together"));
	BOOST_TEST(contains(lines, "res://sky.desc  ->  res://clouds.desc"));
	BOOST_TEST(contains(lines, "res://sky.desc.resource  ->  res://clouds.desc.resource"), "the .resource goes with it");
	BOOST_TEST(contains(lines, "res://levels/main.desc"));
	BOOST_TEST(contains(lines, "res://materials/child.desc"));
	BOOST_TEST(contains(lines, "project.snkproj"), "the manifest names it too");
	BOOST_TEST(contains(lines, "/worlds/0/children/0/components/sky"), "the pointers of each file");
	BOOST_TEST(contains(lines, "reference(s) in"));
}

BOOST_AUTO_TEST_CASE(AMovePreview_ShowsTheFolderTheFileGoesTo_AndStartsWithItsFolder)
{
	open();
	form.open(edt::asset_operation::move, res::tag::make("materials/child.desc"));
	BOOST_TEST(form.input() == "materials");
	form.set_input("env");
	BOOST_REQUIRE(form.item());
	BOOST_TEST(contains(form.preview_lines(), "Move res://materials/child.desc  ->  res://env/child.desc"));
	BOOST_TEST(form.can_submit());

	form.set_input("nowhere");
	BOOST_TEST(!form.can_submit());
	BOOST_REQUIRE(form.refused());
	BOOST_TEST((form.refused()->code == edt::refusal_code::no_such_folder));
	BOOST_TEST(contains(form.refusal_text(), "That folder does not exist"));
	BOOST_TEST(form.preview_lines().empty());
}

BOOST_AUTO_TEST_CASE(ARefusal_IsShownAsPlainTextPerItsCode_WithTheReasonUnderIt)
{
	open();
	form.open(edt::asset_operation::rename, res::tag::make("sky.desc"));
	form.set_input("other.desc");
	BOOST_REQUIRE(form.refused());
	BOOST_TEST((form.refused()->code == edt::refusal_code::target_exists));
	BOOST_TEST(contains(form.refusal_text(), "A file is there already under that name."));
	BOOST_TEST(contains(form.refusal_text(), "res://other.desc"), "the reason names the file");

	form.set_input("a/b.desc");
	BOOST_REQUIRE(form.refused());
	BOOST_TEST(contains(form.refusal_text(), "That is not a name the project can hold."));

	form.open(edt::asset_operation::rename, res::tag::make("missing.desc"));
	BOOST_TEST(contains(form.refusal_text(), "The file is not in the project."));

	engine.write("shared.desc", canonical(R"({ "__type": "material_desc" })"));
	open();
	form.open(edt::asset_operation::rename, res::tag::make("shared.desc"));
	form.set_input("renamed.desc");
	BOOST_TEST(contains(form.refusal_text(), "belongs to the engine and is never changed"));
	BOOST_TEST(!form.can_submit());
	BOOST_TEST(!form.submit());
}

BOOST_AUTO_TEST_CASE(EveryRefusalCode_HasItsOwnSentence)
{
	std::set<std::string> sentences;
	int codes = 0;
	for (int value = 0; value <= static_cast<int>(edt::refusal_code::too_many_files); ++value, ++codes) {
		edt::refusal why;
		why.code = static_cast<edt::refusal_code>(value);
		const std::string text = edt::refusal_message(why);
		BOOST_TEST(!text.empty(), std::to_string(value));
		sentences.insert(text);
	}
	BOOST_TEST(sentences.size() == static_cast<std::size_t>(codes), "no two codes share a text");
}

BOOST_AUTO_TEST_CASE(ADocumentWithUnsavedChangesThatReferToTheFile_IsNamed_AndNothingOffersToGoOn)
{
	open();
	open_files.insert("res://levels/main.desc");
	form.open(edt::asset_operation::rename, res::tag::make("sky.desc"));
	form.set_input("clouds.desc");
	BOOST_REQUIRE(form.refused());
	BOOST_TEST((form.refused()->code == edt::refusal_code::referrer_open));
	const std::string text = form.refusal_text();
	BOOST_TEST(contains(text, "A document with unsaved changes is open"));
	BOOST_TEST(contains(text, "Open with unsaved changes:"));
	BOOST_TEST(contains(text, "res://levels/main.desc"), "the document is named");
	BOOST_TEST(!form.can_submit());
	const auto before = snapshot();
	BOOST_TEST(!form.submit());
	BOOST_TEST((snapshot() == before));

	open_files.clear();
	open_files.insert("res://sky.desc");
	form.open(edt::asset_operation::rename, res::tag::make("sky.desc"));
	form.set_input("clouds.desc");
	BOOST_TEST(contains(form.refusal_text(), "res://sky.desc"));
	BOOST_TEST(!form.can_submit(), "the file itself open with unsaved changes");

	form.open(edt::asset_operation::remove, res::tag::make("sky.desc"));
	BOOST_REQUIRE(form.refused());
	BOOST_TEST((form.refused()->code == edt::refusal_code::referrer_open));
	BOOST_TEST(!form.can_submit());
	BOOST_TEST(!form.can_ask_force(), "force does not bypass an open document");
	form.ask_force();
	BOOST_TEST((form.current() == edt::asset_operation_form::stage::editing));
	BOOST_TEST(!form.submit_force());
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(ADeleteWithReferrers_HasNoDefaultAction_ForceIsAnotherQuestion_ThatListsWhatBreaks)
{
	open();
	form.open(edt::asset_operation::remove, res::tag::make("sky.desc"));
	BOOST_REQUIRE(form.item());
	BOOST_TEST(!form.refused());
	BOOST_TEST(form.needs_force());
	BOOST_TEST(!form.can_submit(), "the default action is off");
	BOOST_TEST(form.can_ask_force());
	BOOST_TEST(contains(form.preview_lines(), "place(s) refer to it"));
	BOOST_TEST(contains(form.preview_lines(), "res://levels/main.desc"));

	const auto before = snapshot();
	BOOST_TEST(!form.submit());
	BOOST_TEST(!form.submit_force(), "the second question was not asked yet");
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(history.size() == 0u);

	form.ask_force();
	BOOST_TEST((form.current() == edt::asset_operation_form::stage::confirming_force));
	const std::string question = form.force_text();
	BOOST_TEST(contains(question, "FORCE"));
	BOOST_TEST(contains(question, "will be broken"));
	BOOST_TEST(contains(question, "res://levels/main.desc"));
	BOOST_TEST(contains(question, "/worlds/0/children/0/components/sky"), "the pointer of each reference");
	BOOST_TEST(contains(question, "res://materials/child.desc"));
	BOOST_TEST(!form.submit(), "the first modal's action does nothing in the second stage");
	BOOST_TEST((snapshot() == before));

	form.back();
	BOOST_TEST((form.current() == edt::asset_operation_form::stage::editing));
	BOOST_TEST(!form.submit_force(), "after Back nothing is confirmed");
	BOOST_TEST((snapshot() == before));

	form.ask_force();
	BOOST_REQUIRE_MESSAGE(form.submit_force(), form.error());
	BOOST_TEST((form.current() == edt::asset_operation_form::stage::reported));
	BOOST_TEST(!has("sky.desc"));
	BOOST_TEST(!form.result().report.broken_references.empty());
	BOOST_TEST(contains(form.report_lines(), "Broken references"));
	BOOST_TEST(history.size() == 1u);
}

BOOST_AUTO_TEST_CASE(TheForceQuestion_ListsAtMostTwelvePlaces_AndCountsTheRest)
{
	write("popular.desc", canonical(R"({ "__type": "material_desc" })"));
	write("popular.desc.resource", meta_text(k_popular_guid));
	for (int i = 0; i < 15; ++i) {
		write(std::format("refs/r{:02}.desc", i), canonical(R"({ "__type": "material_desc", "__parent": "res://popular.desc" })"));
	}
	open();
	form.open(edt::asset_operation::remove, res::tag::make("popular.desc"));
	BOOST_REQUIRE(form.item());
	BOOST_TEST(form.item()->referrers.size() == 15u);
	BOOST_TEST(contains(form.preview_lines(), "15 place(s) refer to it"));
	BOOST_TEST(contains(form.preview_lines(), "... and 3 more"));
	form.ask_force();
	const std::string question = form.force_text();
	BOOST_TEST(contains(question, "15 place(s) still refer to this asset"));
	BOOST_TEST(contains(question, "... and 3 more"));
	BOOST_TEST(contains(question, "res://refs/r11.desc"));
	BOOST_TEST(!contains(question, "res://refs/r12.desc"), "past the cap");
}

BOOST_AUTO_TEST_CASE(ADeleteNothingRefersTo_RunsFromTheFirstWindow_AndItsReportCanBeUndone)
{
	open();
	const auto before = snapshot();
	form.open(edt::asset_operation::remove, res::tag::make("lonely.desc"));
	BOOST_TEST(form.can_submit());
	BOOST_TEST(!form.needs_force());
	BOOST_REQUIRE_MESSAGE(form.submit(), form.error());
	BOOST_TEST((form.current() == edt::asset_operation_form::stage::reported));
	BOOST_TEST(!has("lonely.desc"));
	BOOST_TEST(form.report_title() == "Deleted res://lonely.desc");
	BOOST_TEST(contains(form.report_lines(), "(kept in the journal)"));
	BOOST_TEST(history.size() == 1u);
	BOOST_REQUIRE(form.can_undo());

	BOOST_REQUIRE_MESSAGE(form.undo(), form.undo_error());
	BOOST_TEST(form.undone());
	BOOST_TEST(!form.can_undo());
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(history.size() == 0u);
}

BOOST_AUTO_TEST_CASE(ARenameReport_ListsWhatMovedAndWhatWasRewrittenWithPointers_AndUndoPutsItBack)
{
	open();
	const auto before = snapshot();
	form.open(edt::asset_operation::rename, res::tag::make("sky.desc"));
	form.set_input("clouds.desc");
	BOOST_REQUIRE_MESSAGE(form.submit(), form.error());
	BOOST_TEST(!form.result().report.operation_id.empty());
	BOOST_TEST(history.size() == 1u, "one entry of the history");
	BOOST_TEST(form.report_title() == "Renamed res://sky.desc");
	const std::vector<std::string> lines = form.report_lines();
	BOOST_TEST(contains(lines, "Moved (2):"));
	BOOST_TEST(contains(lines, "res://sky.desc  ->  res://clouds.desc"));
	BOOST_TEST(contains(lines, "Rewritten ("));
	BOOST_TEST(contains(lines, "res://levels/main.desc"));
	BOOST_TEST(contains(lines, "project.snkproj"));
	BOOST_TEST(contains(lines, "/worlds/0/children/0/components/sky"));
	BOOST_TEST(has("clouds.desc"));
	BOOST_TEST(contains(file_text("levels/main.desc"), "res://clouds.desc"));

	BOOST_REQUIRE_MESSAGE(form.undo(), form.undo_error());
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(history.size() == 0u);
}

BOOST_AUTO_TEST_CASE(AnUndoThatIsRefused_ShowsItsTextInTheReport_AndTheFilesStayAsTheyAre)
{
	open();
	form.open(edt::asset_operation::rename, res::tag::make("sky.desc"));
	form.set_input("clouds.desc");
	BOOST_REQUIRE_MESSAGE(form.submit(), form.error());
	settle();
	write("clouds.desc", canonical(R"({ "__type": "material_desc", "edited": true })"));
	const auto after_edit = snapshot();

	BOOST_TEST(!form.undo());
	BOOST_TEST(!form.undo_error().empty());
	BOOST_TEST(!form.undone());
	BOOST_TEST(form.can_undo(), "the history keeps the entry: the button stays to try again");
	BOOST_TEST((snapshot() == after_edit), "nothing was touched");
	BOOST_TEST(!form.undo());
	BOOST_TEST(!form.undo());
	BOOST_TEST(!form.can_undo(), "dropped from the history after the third failure");
	BOOST_TEST(contains(form.undo_error(), "dropped from the history"));

	form.open(edt::asset_operation::rename, res::tag::make("lonely.desc"));
	form.set_input("alone.desc");
	BOOST_REQUIRE_MESSAGE(form.submit(), form.error());
	BOOST_TEST(form.undo_error().empty(), "a new report starts without the old refusal");
}

BOOST_AUTO_TEST_CASE(AnOperationTheGuardsRefuse_StaysInTheWindowWithItsReason)
{
	open();
	form.open(edt::asset_operation::rename, res::tag::make("sky.desc"));
	form.set_input("clouds.desc");
	blocked = "a migration is running";
	BOOST_TEST(!form.submit());
	BOOST_TEST((form.current() == edt::asset_operation_form::stage::editing));
	BOOST_TEST(contains(form.error(), "a migration is running"));
	BOOST_TEST(has("sky.desc"));
	blocked.reset();
	BOOST_TEST(form.submit());
	BOOST_TEST(form.error().empty());
}

BOOST_AUTO_TEST_CASE(TheManifestCopy_IsReadAgainAfterAnOperationAndAfterItsUndo_OnlyInItsTwoReferences)
{
	open();
	prj::manifest in_memory;
	in_memory.name = "kept";
	in_memory.start_scene = "res://sky.desc";
	in_memory.input_actions = "res://input.desc";
	BOOST_TEST(edt::refresh_manifest_references(manifest_file, in_memory).empty());
	BOOST_TEST(in_memory.start_scene == "res://sky.desc");
	BOOST_TEST(in_memory.input_actions.empty(), "the file names none");
	BOOST_TEST(in_memory.name == "kept", "nothing else is taken from the file");

	form.open(edt::asset_operation::rename, res::tag::make("sky.desc"));
	form.set_input("clouds.desc");
	BOOST_REQUIRE_MESSAGE(form.submit(), form.error());
	BOOST_TEST(contains(manifest_text(), "res://clouds.desc"));
	BOOST_TEST(manifest.start_scene == "res://clouds.desc", "refreshed by the operation's done callback");
	BOOST_TEST(edt::refresh_manifest_references(manifest_file, in_memory).empty());
	BOOST_TEST(in_memory.start_scene == "res://clouds.desc");

	BOOST_REQUIRE_MESSAGE(form.undo(), form.undo_error());
	BOOST_TEST(manifest.start_scene == "res://sky.desc", "refreshed after the undo");
}

BOOST_AUTO_TEST_CASE(TheManifestRefresh_LeavesTheCopyAsItWas_WhenTheFileCannotBeRead)
{
	prj::manifest data;
	data.start_scene = "res://kept.desc";
	BOOST_TEST(!edt::refresh_manifest_references(manifest_dir.path() / "gone.snkproj", data).empty());
	manifest_dir.write("bad.snkproj", "{ not json");
	BOOST_TEST(!edt::refresh_manifest_references(manifest_dir.path() / "bad.snkproj", data).empty());
	BOOST_TEST(data.start_scene == "res://kept.desc");
}

BOOST_AUTO_TEST_CASE(AfterAForcedDelete_TheBrokenReferencesAreRowsOfProblems_AndUndoTakesThemAway)
{
	open();
	publisher.publish(problems, *db);
	BOOST_TEST(broken_rows().empty(), "nothing is broken before");

	form.open(edt::asset_operation::remove, res::tag::make("sky.desc"));
	form.ask_force();
	BOOST_REQUIRE_MESSAGE(form.submit_force(), form.error());
	settle();
	publisher.publish(problems, *db);
	const std::vector<edt::problem> broken = broken_rows();
	BOOST_REQUIRE(!broken.empty());
	bool level = false;
	bool child = false;
	bool guid_error = false;
	for (const edt::problem& row : broken) {
		level = level || row.file == "res://levels/main.desc";
		child = child || row.file == "res://materials/child.desc";
		guid_error = guid_error || (row.severity == edt::problem_severity::error && contains(row.message, std::string(k_sky_guid)));
		BOOST_TEST(contains(row.message, "which is not in the project"));
	}
	BOOST_TEST(level);
	BOOST_TEST(child);
	BOOST_TEST(guid_error, "a {guid, path} whose guid no file holds is an error");

	BOOST_REQUIRE_MESSAGE(form.undo(), form.undo_error());
	settle();
	publisher.publish(problems, *db);
	BOOST_TEST(broken_rows().empty());
}

BOOST_AUTO_TEST_CASE(AfterAFailedRun_TheListIsAskedToRefresh_AndThePlanIsMadeAgain)
{
	open();
	form.open(edt::asset_operation::rename, res::tag::make("sky.desc"));
	form.set_input("clouds.desc");
	BOOST_TEST(!form.take_disk_changed(), "nothing ran yet");
	const int planned = plan_calls;

	fail_prefix = "rewrite:";
	BOOST_TEST(!form.submit());
	BOOST_TEST((form.result().state == edt::asset_run_state::failed));
	BOOST_TEST(!form.error().empty());
	BOOST_TEST(form.take_disk_changed(), "files may have moved before the failure");
	BOOST_TEST(!form.take_disk_changed(), "asked once");
	BOOST_TEST(plan_calls == planned + 1, "the form plans again for the disk as it is now");
	BOOST_TEST((form.current() == edt::asset_operation_form::stage::editing));
	BOOST_TEST(form.item() != nullptr);
}

BOOST_AUTO_TEST_CASE(AfterARefusalOfTheRun_TheListIsAskedToRefresh_AndThePlanIsMadeAgain)
{
	open();
	form.open(edt::asset_operation::rename, res::tag::make("sky.desc"));
	form.set_input("clouds.desc");
	write("levels/main.desc", canonical(R"({ "__type": "level_desc" })"));
	const int planned = plan_calls;

	BOOST_TEST(!form.submit());
	BOOST_TEST((form.result().state == edt::asset_run_state::refused));
	BOOST_TEST(form.take_disk_changed());
	BOOST_TEST(plan_calls == planned + 1, "planned again after the refusal");
}

BOOST_AUTO_TEST_CASE(ABlockedRun_ChangesNothing_AndPlansNothingAgain)
{
	open();
	form.open(edt::asset_operation::rename, res::tag::make("sky.desc"));
	form.set_input("clouds.desc");
	blocked = "a migration is running";
	const int planned = plan_calls;
	BOOST_TEST(!form.submit());
	BOOST_TEST(!form.take_disk_changed());
	BOOST_TEST(plan_calls == planned);
}

BOOST_AUTO_TEST_CASE(AnUndo_AsksForARefreshWhetherOrNotItWorked)
{
	open();
	form.open(edt::asset_operation::rename, res::tag::make("lonely.desc"));
	form.set_input("alone.desc");
	BOOST_REQUIRE(form.submit());
	BOOST_TEST(form.take_disk_changed());
	BOOST_REQUIRE(form.undo());
	BOOST_TEST(form.take_disk_changed());
}

BOOST_AUTO_TEST_CASE(TypingPlansOnlyOnceItStopped_AndNothingCanBeSubmittedMeanwhile)
{
	open();
	form.open(edt::asset_operation::rename, res::tag::make("sky.desc"));
	const auto start = std::chrono::steady_clock::now();
	const int planned = plan_calls;

	form.type_input("clouds.desc", start);
	form.type_input("clouds2.desc", start + std::chrono::milliseconds(100));
	BOOST_TEST(form.planning_pending());
	BOOST_TEST(!form.can_submit());
	BOOST_TEST(!form.submit());
	form.settle(start + std::chrono::milliseconds(300));
	BOOST_TEST(form.planning_pending(), "the last key was 200 ms ago");
	BOOST_TEST(plan_calls == planned);

	form.settle(start + std::chrono::milliseconds(100) + edt::asset_operation_form::input_delay);
	BOOST_TEST(!form.planning_pending());
	BOOST_TEST(plan_calls == planned + 1, "one plan for the two keys");
	BOOST_TEST(form.can_submit());
	BOOST_TEST(contains(form.preview_lines(), "res://clouds2.desc"));

	form.type_input("other.desc", start);
	BOOST_TEST(!form.can_submit());
	form.set_input("other.desc");
	BOOST_TEST(!form.planning_pending(), "Enter plans at once");
	BOOST_TEST(plan_calls == planned + 2);
}

BOOST_AUTO_TEST_CASE(TheAssetsName_IsShownEscaped)
{
	open();
	form.open(edt::asset_operation::rename, res::tag::make("we\nird.desc"));
	const std::string shown = form.asset_text();
	BOOST_TEST(shown.find('\n') == std::string::npos);
	BOOST_TEST(contains(shown, "\\n"));
}

BOOST_AUTO_TEST_SUITE_END()
