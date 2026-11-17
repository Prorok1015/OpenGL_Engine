#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_database.h"
#include "edt_asset_ops.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_pretty_json.h>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
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

// Rename, move and delete of one asset (#775, EPIC-57 section 6.4): the plan, the references that follow, the journal that
// takes everything back.
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

constexpr std::string_view k_sky_guid = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";
constexpr std::string_view k_other_guid = "5b1d2e3f-4a5b-4c6d-8e7f-0a1b2c3d4e5f";
constexpr std::string_view k_lonely_guid = "6c2e3f40-5b6c-4d7e-8f80-1b2c3d4e5f60";
constexpr std::string_view k_rock_guid = "7d3f4051-6c7d-4e8f-8091-2c3d4e5f6071";
constexpr std::string_view k_rock_bin_guid = "8e405162-7d8e-4f90-81a2-3d4e5f607182";
constexpr std::string_view k_shadow_guid = "9f516273-8e9f-4a01-92b3-4e5f60718293";

std::string meta_text(std::string_view guid)
{
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(guid);
	meta.importer = "test";
	return res::write_asset_meta(meta);
}

// A file in the layout the project writes, which is the only one the rewriter edits.
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
			"lost": { "__type": "mesh_node_desc", "material": { "guid": "00000000-0000-4000-8000-000000000009", "path": "res://sky.desc" } },
			"other": { "__type": "mesh_node_desc", "material": "res://other.desc" },
			"foreign": { "__type": "mesh_node_desc", "material": { "guid": "5b1d2e3f-4a5b-4c6d-8e7f-0a1b2c3d4e5f", "path": "res://sky.desc" } }
		} } ] } ],
	"web": "https://example.com/sky.desc"
})";

const std::string k_node = "/worlds/0/children/0/components";

struct ops_fixture
{
	ops_fixture() : layer("asset_ops"), engine("asset_ops_engine"), cache("asset_ops_cache"), journal("asset_ops_journal")
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
		write("meshes/rock.mesh.desc", canonical(R"({ "__type": "mesh_desc" })"));
		write("meshes/rock.mesh.desc.resource", meta_text(k_rock_guid));
		write("meshes/rock.mesh.bin", std::string("SNKB\x01\x00\x00\x00 geometry", 17));
		write("meshes/rock.mesh.bin.resource", meta_text(k_rock_bin_guid));
		write("levels/rocks.desc", canonical(R"({ "__type": "level_desc", "rock": "res://meshes/rock.mesh.desc" })"));
		write("shadow.desc", canonical(R"({ "__type": "material_desc" })"));
		write("shadow.desc.resource", meta_text(k_shadow_guid));
		engine.write("engine_mat.desc", canonical(R"({ "__type": "material_desc" })"));
		engine.write("shadow.desc", canonical(R"({ "__type": "material_desc" })"));
	}

	// Older than the stamp granularity, so a cache trusts what it learned of the file.
	void write(std::string_view relative, std::string_view text)
	{
		const fs::path path = layer.write(relative, text);
		fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(1));
	}

	edt::asset_database& open(bool with_cache = false, bool case_insensitive = edt::platform_case_insensitive)
	{
		db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ engine.path(), layer.path() },
			with_cache ? edt::asset_cache_location{ cache.path(), "asset_db" } : edt::asset_cache_location{});
		db->set_case_insensitive(case_insensitive);
		db->rebuild();
		test_util::pump(tasks);
		return *db;
	}

	fs::path cache_file() const { return cache.path() / "asset_db" / "assets.json"; }

	json::value read_cache() const
	{
		std::ifstream in(cache_file(), std::ios::binary);
		return json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
	}

	void write_cache(const json::value& document) const
	{
		std::ofstream(cache_file(), std::ios::binary | std::ios::trunc) << json::serialize(document);
	}

	edt::asset_ops ops()
	{
		edt::asset_ops_ports ports;
		ports.assets = [this]() -> const edt::asset_database* { return db.get(); };
		ports.engine_base = engine.path();
		ports.journal_root = journal.path();
		ports.is_open_with_unsaved_changes = [this](const res::tag& tag) { return open_files.contains(std::string(tag.view())); };
		ports.hold_changes = [this]() -> std::shared_ptr<void> {
			++holds;
			return std::make_shared<int>(0);
		};
		ports.fail_step = [this](std::string_view step) -> std::string {
			if (step != fail_at) return {};
			return "injected failure";
		};
		return edt::asset_ops(std::move(ports));
	}

	static edt::asset_request request(edt::asset_operation operation, std::string_view asset, std::string_view to = {})
	{
		return edt::asset_request{ operation, res::tag::make(asset), std::string(to) };
	}

	edt::operation_plan plan(const edt::asset_request& asked)
	{
		return ops().plan(std::span<const edt::asset_request>(&asked, 1));
	}

	edt::operation_report run(const edt::asset_request& asked, bool force = false)
	{
		edt::asset_ops operations = ops();
		const edt::operation_plan planned = operations.plan(std::span<const edt::asset_request>(&asked, 1));
		return operations.execute(planned, edt::execute_options{ force });
	}

	std::string text_of(std::string_view relative) const
	{
		std::ifstream in(layer.path() / std::string(relative), std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	bool has(std::string_view relative) const { return fs::is_regular_file(layer.path() / std::string(relative)); }

	// Every file of the project's own folder with its bytes: what a rollback must give back.
	std::map<std::string, std::string> snapshot() const
	{
		std::map<std::string, std::string> out;
		for (const fs::directory_entry& entry : fs::recursive_directory_iterator(layer.path())) {
			if (!entry.is_regular_file()) continue;
			out[entry.path().lexically_relative(layer.path()).generic_string()] = text_of(entry.path().lexically_relative(layer.path()).generic_string());
		}
		return out;
	}

	static const edt::rewritten_file* rewritten_in(const edt::operation_report& report, std::string_view file)
	{
		const auto found = std::ranges::find(report.rewritten, file, &edt::rewritten_file::file);
		return found == report.rewritten.end() ? nullptr : &*found;
	}

	static bool contains(const std::vector<std::string>& pointers, std::string_view pointer)
	{
		return std::ranges::find(pointers, pointer) != pointers.end();
	}

	test_util::temp_dir layer;
	test_util::temp_dir engine;
	test_util::temp_dir cache;
	test_util::temp_dir journal;
	ds::task_system tasks{ test_util::manual_tasks() };
	edt::editor_events events;
	edt::job_service jobs{ tasks, &events };
	std::unique_ptr<edt::asset_database> db;
	std::set<std::string> open_files;
	std::string fail_at;
	int holds = 0;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtAssetOpsTests, ops_fixture)

BOOST_AUTO_TEST_CASE(ARename_KeepsEveryFormOfReference_AndTheReportListsWhatWasRewritten)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(holds == 1, "the watcher reports the operation as one batch");

	BOOST_TEST(!has("sky.desc"));
	BOOST_TEST(has("clouds.desc"));
	BOOST_TEST(text_of("clouds.desc.resource") == meta_text(k_sky_guid), "the .resource goes with its file, unchanged");
	BOOST_TEST(!has("sky.desc.resource"));
	BOOST_REQUIRE(report.moved.size() == 2u);
	BOOST_TEST(report.moved[0].from == "res://sky.desc");
	BOOST_TEST(report.moved[0].to == "res://clouds.desc");
	BOOST_TEST(report.moved[1].from == "res://sky.desc.resource");
	BOOST_TEST(report.moved[1].to == "res://clouds.desc.resource");

	const json::value level = json::parse(text_of("levels/main.desc"));
	const json::value& components = level.at("worlds").as_array().at(0).at("children").as_array().at(0).at("components");
	BOOST_TEST(std::string(components.at("sky").as_string().c_str()) == "res://clouds.desc", "a path-only string");
	const json::value& mesh = components.at("mesh").at("material");
	BOOST_TEST(std::string(mesh.at("path").as_string().c_str()) == "res://clouds.desc", "an object: the path");
	BOOST_TEST(std::string(mesh.at("guid").as_string().c_str()) == k_sky_guid, "an object: the guid stays");
	BOOST_TEST(std::string(components.at("inst").at("__parent").as_string().c_str()) == "res://clouds.desc");
	BOOST_TEST(std::string(components.at("lost").at("material").at("path").as_string().c_str()) == "res://clouds.desc");
	BOOST_TEST(std::string(components.at("foreign").at("material").at("path").as_string().c_str()) == "res://sky.desc", "another asset's reference stays");
	BOOST_TEST(std::string(components.at("other").at("material").as_string().c_str()) == "res://other.desc");
	BOOST_TEST(std::string(level.at("web").as_string().c_str()) == "https://example.com/sky.desc");
	BOOST_TEST(text_of("levels/main.desc") == canonical(text_of("levels/main.desc")), "still in the project's layout");

	const json::value child = json::parse(text_of("materials/child.desc"));
	BOOST_TEST(std::string(child.at("__parent").as_string().c_str()) == "res://clouds.desc");

	BOOST_REQUIRE(report.rewritten.size() == 2u);
	const edt::rewritten_file* main = rewritten_in(report, "res://levels/main.desc");
	BOOST_REQUIRE(main);
	BOOST_TEST(main->json_pointers.size() == 4u);
	BOOST_TEST(contains(main->json_pointers, k_node + "/sky"));
	BOOST_TEST(contains(main->json_pointers, k_node + "/mesh/material"));
	BOOST_TEST(contains(main->json_pointers, k_node + "/inst/__parent"));
	BOOST_TEST(contains(main->json_pointers, k_node + "/lost/material"));
	const edt::rewritten_file* inherited = rewritten_in(report, "res://materials/child.desc");
	BOOST_REQUIRE(inherited);
	BOOST_TEST((inherited->json_pointers == std::vector<std::string>{ "/__parent" }));
	BOOST_TEST(report.could_not_rewrite.empty());
	BOOST_TEST(report.broken_references.empty());
}

BOOST_AUTO_TEST_CASE(AMove_GoesToAnotherFolder_AndTheReferencesFollow)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::move, "sky.desc", "env"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(has("env/sky.desc"));
	BOOST_TEST(has("env/sky.desc.resource"));
	BOOST_TEST(!has("sky.desc"));
	const json::value child = json::parse(text_of("materials/child.desc"));
	BOOST_TEST(std::string(child.at("__parent").as_string().c_str()) == "res://env/sky.desc");
}

BOOST_AUTO_TEST_CASE(ADescWithABinaryFile_MovesAsAPairOfPairs_AndNamedReferencesFollow)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "meshes/rock.mesh.desc", "stone.mesh.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_REQUIRE(report.moved.size() == 4u);
	BOOST_TEST(has("meshes/stone.mesh.desc"));
	BOOST_TEST(has("meshes/stone.mesh.desc.resource"));
	BOOST_TEST(has("meshes/stone.mesh.bin"));
	BOOST_TEST(has("meshes/stone.mesh.bin.resource"));
	BOOST_TEST(!has("meshes/rock.mesh.bin"));
	BOOST_TEST(text_of("meshes/stone.mesh.bin.resource") == meta_text(k_rock_bin_guid));
	const json::value rocks = json::parse(text_of("levels/rocks.desc"));
	BOOST_TEST(std::string(rocks.at("rock").as_string().c_str()) == "res://meshes/stone.mesh.desc");

	const auto listed = [&report](std::string_view from, std::string_view to) {
		return std::ranges::any_of(report.moved, [&](const edt::file_move& moved) { return moved.from == from && moved.to == to; });
	};
	BOOST_TEST(listed("res://meshes/rock.mesh.bin", "res://meshes/stone.mesh.bin"));
	BOOST_TEST(listed("res://meshes/rock.mesh.bin.resource", "res://meshes/stone.mesh.bin.resource"));
}

BOOST_AUTO_TEST_CASE(ARenameThatWouldLoseTheBinaryFilesName_IsRefused)
{
	open();
	const edt::operation_plan planned = plan(request(edt::asset_operation::rename, "meshes/rock.mesh.desc", "rock.desc"));
	BOOST_REQUIRE(planned.refused.has_value());
	BOOST_TEST((planned.refused->code == edt::refusal_code::binary_name_lost));
	BOOST_TEST(has("meshes/rock.mesh.desc"));
}

BOOST_AUTO_TEST_CASE(AFileOfTheEngineLayer_IsRefused)
{
	open();
	for (const edt::asset_operation operation : { edt::asset_operation::rename, edt::asset_operation::move, edt::asset_operation::remove }) {
		const edt::operation_plan planned = plan(request(operation, "engine_mat.desc", operation == edt::asset_operation::move ? "env" : "x.desc"));
		BOOST_REQUIRE(planned.refused.has_value());
		BOOST_TEST((planned.refused->code == edt::refusal_code::engine_layer), edt::asset_operation_name(operation));
		BOOST_TEST(!planned.refused->reason.empty());
	}
	BOOST_TEST(std::string(edt::refusal_code_name(edt::refusal_code::engine_layer)) == "engine_layer");
}

BOOST_AUTO_TEST_CASE(ACaseOnlyRename_IsRefusedWhereNamesIgnoreCase_AndAllowedWhereTheyDoNot)
{
	open(false, true);
	const edt::operation_plan refused = plan(request(edt::asset_operation::rename, "sky.desc", "Sky.desc"));
	BOOST_REQUIRE(refused.refused.has_value());
	BOOST_TEST((refused.refused->code == edt::refusal_code::case_only_rename));

	if (edt::platform_case_insensitive) return;
	open(false, false);
	const edt::operation_plan allowed = plan(request(edt::asset_operation::rename, "sky.desc", "Sky.desc"));
	BOOST_TEST(!allowed.refused.has_value());
}

BOOST_AUTO_TEST_CASE(ANameThatIsTaken_Refuses)
{
	open();
	const edt::operation_plan planned = plan(request(edt::asset_operation::rename, "sky.desc", "other.desc"));
	BOOST_REQUIRE(planned.refused.has_value());
	BOOST_TEST((planned.refused->code == edt::refusal_code::target_exists));

	const edt::operation_plan engine_name = plan(request(edt::asset_operation::rename, "sky.desc", "engine_mat.desc"));
	BOOST_REQUIRE_MESSAGE(engine_name.refused.has_value(), "a name of the engine's file is taken too");
	BOOST_TEST((engine_name.refused->code == edt::refusal_code::target_exists));

	const edt::operation_plan again = plan(request(edt::asset_operation::rename, "sky.desc", "sky.desc"));
	BOOST_REQUIRE(again.refused.has_value());
	BOOST_TEST((again.refused->code == edt::refusal_code::no_change));
}

BOOST_AUTO_TEST_CASE(ANameThatIsNotPortable_AndAFolderThatIsNotThere_AreRefused)
{
	open();
	for (const std::string_view name : { "con.desc", "a:b.desc", "ends.", "x/y.desc" }) {
		const edt::operation_plan planned = plan(request(edt::asset_operation::rename, "sky.desc", name));
		BOOST_REQUIRE_MESSAGE(planned.refused.has_value(), name);
		BOOST_TEST((planned.refused->code == edt::refusal_code::unportable_path || planned.refused->code == edt::refusal_code::bad_name), name);
	}
	const edt::operation_plan moved = plan(request(edt::asset_operation::move, "sky.desc", "no/such"));
	BOOST_REQUIRE(moved.refused.has_value());
	BOOST_TEST((moved.refused->code == edt::refusal_code::no_such_folder));
	const edt::operation_plan unknown = plan(request(edt::asset_operation::rename, "missing.desc", "b.desc"));
	BOOST_REQUIRE(unknown.refused.has_value());
	BOOST_TEST((unknown.refused->code == edt::refusal_code::not_found));
	const edt::asset_request two[] = { request(edt::asset_operation::remove, "sky.desc"), request(edt::asset_operation::remove, "other.desc") };
	BOOST_TEST(((ops().plan(two).refused->code) == edt::refusal_code::one_asset_only));
}

BOOST_AUTO_TEST_CASE(AUsageScanThatSkippedAFile_RefusesARenameAndAMove_ButNotAForcedDelete)
{
	open(true).save();
	db.reset();
	json::value document = read_cache();
	for (json::value& record : document.at("records").as_array()) {
		if (std::string_view(record.at("path").as_string().c_str()) != "res://levels/main.desc") continue;
		record.as_object().erase("references");
		record.as_object()["references_skipped"] = true;
	}
	write_cache(document);
	BOOST_REQUIRE(open(true).loaded_from_cache());

	const edt::operation_plan renamed = plan(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE(renamed.refused.has_value());
	BOOST_TEST((renamed.refused->code == edt::refusal_code::usage_scan_skipped));
	BOOST_TEST((renamed.refused->skipped == std::vector<std::string>{ "res://levels/main.desc" }));
	BOOST_TEST(has("sky.desc"));
	BOOST_TEST(((plan(request(edt::asset_operation::move, "sky.desc", "env")).refused->code) == edt::refusal_code::usage_scan_skipped));

	const edt::operation_report unforced = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE(unforced.refused.has_value());
	BOOST_TEST((unforced.refused->code == edt::refusal_code::usage_scan_skipped));
	BOOST_TEST(has("lonely.desc"));
	const edt::operation_report forced = run(request(edt::asset_operation::remove, "lonely.desc"), true);
	BOOST_REQUIRE_MESSAGE(forced.ok, forced.error);
	BOOST_TEST(!forced.warnings.empty());
	BOOST_TEST(!has("lonely.desc"));
}

BOOST_AUTO_TEST_CASE(AReferrerOpenWithUnsavedChanges_RefusesTheOperation_AndNothingIsTouched)
{
	open();
	const auto before = snapshot();
	open_files.insert("res://levels/main.desc");
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE(report.refused.has_value());
	BOOST_TEST((report.refused->code == edt::refusal_code::referrer_open));
	BOOST_TEST(!report.refused->referrers.empty());
	BOOST_TEST(report.refused->referrers.front().file == "res://levels/main.desc");
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(holds == 0);

	open_files = { "res://sky.desc" };
	BOOST_TEST(((plan(request(edt::asset_operation::rename, "sky.desc", "clouds.desc")).refused->code) == edt::refusal_code::referrer_open),
		"the asset itself, open and dirty, is as good a reason");
}

BOOST_AUTO_TEST_CASE(AReferrerThatDoesNotWriteBackUnchanged_IsReportedAndLeftAlone)
{
	write("levels/main.desc", k_level);
	open();
	const std::string untouched = text_of("levels/main.desc");
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(text_of("levels/main.desc") == untouched);
	BOOST_REQUIRE(report.could_not_rewrite.size() == 1u);
	BOOST_TEST(report.could_not_rewrite[0].file == "res://levels/main.desc");
	BOOST_TEST(report.could_not_rewrite[0].code == "not_canonical");
	BOOST_TEST(!report.could_not_rewrite[0].reason.empty());
	BOOST_TEST(!report.broken_references.empty(), "a reference by path alone no longer finds the asset");
	BOOST_TEST(std::ranges::all_of(report.broken_references, [](const edt::asset_usage& usage) { return !usage.by_guid; }));
	BOOST_TEST(rewritten_in(report, "res://materials/child.desc") != nullptr, "the other referrer is rewritten");
}

BOOST_AUTO_TEST_CASE(ADeleteWithReferrersAndNoForce_IsRefusedWithTheirList)
{
	open();
	const auto before = snapshot();
	const edt::operation_report report = run(request(edt::asset_operation::remove, "sky.desc"));
	BOOST_REQUIRE(report.refused.has_value());
	BOOST_TEST((report.refused->code == edt::refusal_code::has_referrers));
	BOOST_TEST(report.refused->referrers.size() == 5u);
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(AForcedDelete_LeavesTheReferrers_AndReportsThemBroken)
{
	open();
	const std::string level = text_of("levels/main.desc");
	const edt::operation_report report = run(request(edt::asset_operation::remove, "sky.desc"), true);
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(report.broken_references.size() == 5u);
	BOOST_TEST(text_of("levels/main.desc") == level);
	BOOST_TEST(report.rewritten.empty());
	BOOST_TEST(!report.warnings.empty());
	BOOST_TEST(!has("sky.desc"));
	BOOST_TEST(!has("sky.desc.resource"));

	BOOST_REQUIRE(report.moved.size() == 2u);
	BOOST_TEST(report.moved[0].to == "cache://asset_ops/" + report.operation_id + "/files/sky.desc");
	const fs::path stash = journal.path() / report.operation_id / "files" / "sky.desc";
	BOOST_TEST(fs::is_regular_file(stash), "the file waits in the journal folder, not erased");
	BOOST_TEST(fs::is_regular_file(journal.path() / report.operation_id / "journal.json"));
}

BOOST_AUTO_TEST_CASE(ADeleteOfAFileNobodyRefersTo_NeedsNoForce_AndUndoPutsItBack)
{
	open();
	const auto before = snapshot();
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(report.broken_references.empty());
	BOOST_TEST(!has("lonely.desc"));
	BOOST_TEST(!has("lonely.desc.resource"));

	const edt::undo_report undone = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(ADeleteOfAFileThatCoveredTheEnginesOne_WarnsThatTheEnginesShowsAgain)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::remove, "shadow.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_REQUIRE(!report.warnings.empty());
	BOOST_TEST(report.warnings[0].find("engine") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AFailureInTheMiddle_TakesEveryStepBack)
{
	open();
	const auto before = snapshot();
	for (const std::string_view step : { "move:sky.desc", "move:sky.desc.resource", "rewrite:levels/main.desc", "rewrite:materials/child.desc" }) {
		fail_at = std::string(step);
		const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
		BOOST_TEST(!report.ok, step);
		BOOST_TEST(!report.refused.has_value(), step);
		BOOST_TEST(report.error == "injected failure", step);
		BOOST_TEST(report.rollback_failures.empty(), step);
		BOOST_TEST((snapshot() == before), std::string("everything is as it was after a failure at ") + std::string(step));
	}
	fail_at = "stash:sky.desc.resource";
	const edt::operation_report removal = run(request(edt::asset_operation::remove, "sky.desc"), true);
	BOOST_TEST(!removal.ok);
	BOOST_TEST(removal.rollback_failures.empty());
	BOOST_TEST((snapshot() == before), "a delete that fails after the first file went is taken back");
}

BOOST_AUTO_TEST_CASE(ARenameChangedUnderThePlan_IsRefusedBeforeAnythingMoves)
{
	open();
	edt::asset_ops operations = ops();
	const edt::asset_request asked = request(edt::asset_operation::rename, "sky.desc", "clouds.desc");
	const edt::operation_plan planned = operations.plan(std::span<const edt::asset_request>(&asked, 1));
	BOOST_REQUIRE(!planned.refused.has_value());
	write("materials/child.desc", canonical(R"({ "__type": "material_desc", "__parent": "res://sky.desc", "note": 1 })"));
	const auto before = snapshot();
	const edt::operation_report report = operations.execute(planned);
	BOOST_REQUIRE(report.refused.has_value());
	BOOST_TEST((report.refused->code == edt::refusal_code::stale_plan));
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(AnUndo_PutsEveryFileAndEveryByteBack)
{
	open();
	const auto before = snapshot();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST((snapshot() != before));

	const std::optional<edt::operation_journal> loaded = edt::journal_from_json(json::parse(json::serialize(edt::journal_to_json(report.journal))));
	BOOST_REQUIRE_MESSAGE(loaded.has_value(), "the journal survives being written as JSON");
	BOOST_TEST(loaded->id == report.journal.id);
	BOOST_TEST(loaded->steps.size() == report.journal.steps.size());

	const edt::undo_report undone = ops().undo(*loaded);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST(undone.restored.size() == 4u);
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(AnUndo_RefusesWhenATouchedFileChangedSince_AndTouchesNothing)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);

	write("levels/main.desc", canonical(R"({ "__type": "level_desc", "edited": true })"));
	const auto after_edit = snapshot();
	const edt::undo_report undone = ops().undo(report.journal);
	BOOST_TEST(!undone.ok);
	BOOST_TEST(!undone.changed_since.empty());
	BOOST_TEST(std::ranges::find(undone.changed_since, "levels/main.desc") != undone.changed_since.end());
	BOOST_TEST((snapshot() == after_edit));

	write("clouds.desc", canonical(R"({ "__type": "material_desc", "edited": true })"));
	const edt::undo_report moved_file = ops().undo(report.journal);
	BOOST_TEST(!moved_file.ok);
	BOOST_TEST(std::ranges::find(moved_file.changed_since, "clouds.desc") != moved_file.changed_since.end());
}

BOOST_AUTO_TEST_CASE(AnEngineLayerHelper_NamesTheLayerThatIsTheEngines)
{
	const std::vector<fs::path> layers{ engine.path(), layer.path() };
	BOOST_TEST(edt::is_engine_layer(layers, 0, engine.path()));
	BOOST_TEST(!edt::is_engine_layer(layers, 1, engine.path()));
	BOOST_TEST(!edt::is_engine_layer(layers, 0, {}), "no engine layer configured");
	BOOST_TEST(!edt::is_engine_layer(layers, 5, engine.path()));
}

BOOST_AUTO_TEST_SUITE_END()
