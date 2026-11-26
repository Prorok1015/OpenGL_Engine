#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_asset_database.h"
#include "edt_asset_history.h"
#include "edt_asset_ops.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/project/prj_manifest.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_pretty_json.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

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
constexpr std::string_view k_kit_a_guid = "a1b2c3d4-1111-4222-8333-444455556666";
constexpr std::string_view k_kit_b_guid = "b2c3d4e5-2222-4333-8444-555566667777";
constexpr std::string_view k_kit_rock_guid = "c3d4e5f6-3333-4444-8555-666677778888";
constexpr std::string_view k_kit_rock_bin_guid = "d4e5f607-4444-4555-8666-777788889999";

std::string meta_text(std::string_view guid)
{
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(guid);
	meta.importer = "test";
	return res::write_asset_meta(meta);
}

// POSIX inode and device of a file, {0, 0} where the platform gives none: the same pair before and after a move means the file was
// renamed, not copied.
std::pair<std::uint64_t, std::uint64_t> identity_of(const fs::path& path)
{
#if !defined(_WIN32)
	struct stat info;
	if (::stat(path.c_str(), &info) != 0) return { 0u, 0u };
	return { static_cast<std::uint64_t>(info.st_ino), static_cast<std::uint64_t>(info.st_dev) };
#else
	(void)path;
	return { 0u, 0u };
#endif
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
			"other": { "__type": "mesh_node_desc", "material": "res://other.desc" },
			"foreign": { "__type": "mesh_node_desc", "material": { "guid": "5b1d2e3f-4a5b-4c6d-8e7f-0a1b2c3d4e5f", "path": "res://sky.desc" } }
		} } ] } ],
	"web": "https://example.com/sky.desc"
})";

const std::string k_node = "/worlds/0/children/0/components";

struct ops_fixture
{
	ops_fixture() : layer("asset_ops"), engine("asset_ops_engine"), lower("asset_ops_lower"), cache("asset_ops_cache"), journal("asset_ops_journal"),
		foreign("asset_ops_foreign")
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

	edt::asset_database& open(bool with_cache = false, bool case_insensitive = edt::platform_case_insensitive, bool with_lower = false)
	{
		std::vector<fs::path> layers{ engine.path() };
		if (with_lower) layers.push_back(lower.path());
		layers.push_back(layer.path());
		db = std::make_unique<edt::asset_database>(jobs, &events, std::move(layers),
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
		ports.journal_root = journal_override.empty() ? journal.path() : journal_override;
		if (!manifest_dir.empty()) {
			ports.manifest = [this]() -> std::optional<edt::asset_ops_manifest> {
				return edt::asset_ops_manifest{ manifest_dir, std::string(prj::k_manifest_file_name), {} };
			};
		}
		ports.is_open_with_unsaved_changes = [this](const res::tag& tag) { return open_files.contains(std::string(tag.view())); };
		ports.hold_changes = [this]() -> std::shared_ptr<void> {
			if (throw_in_hold) throw std::runtime_error("hold refused");
			++holds;
			return std::make_shared<int>(0);
		};
		ports.report_self_change = [this](std::span<const res::file_change> changes) {
			self_changes.insert(self_changes.end(), changes.begin(), changes.end());
			if (forward_self_changes && db) db->report_self_change(changes);
		};
		ports.force_copy = [this](std::string_view) { return copy_instead_of_rename; };
		ports.rename_error = [this](std::string_view) { return rename_error_text; };
		ports.limits = limits;
		ports.operation_started = [this](const edt::operation_journal& started) -> std::string {
			if (throw_when_listed) throw std::runtime_error("the history threw");
			return history ? history->record_pending(started) : std::string{};
		};
		ports.operation_ended = [this](const edt::operation_journal& ended, bool done) {
			if (history) history->end_pending(ended, done);
		};
		ports.fail_step = [this](std::string_view step) -> std::string {
			steps.emplace_back(step);
			if (on_step) on_step(step);
			if (!throw_at.empty() && step == throw_at) throw std::runtime_error("port threw");
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

	// Every file of the project's own folder with its bytes and every folder (a key ending in '/'): what an undo of a folder
	// operation must give back.
	std::map<std::string, std::string> tree() const
	{
		std::map<std::string, std::string> out;
		for (const fs::directory_entry& entry : fs::recursive_directory_iterator(layer.path())) {
			const std::string relative = entry.path().lexically_relative(layer.path()).generic_string();
			if (entry.is_directory()) out[relative + "/"] = {};
			else out[relative] = text_of(relative);
		}
		return out;
	}

	// The folder kit/ (#1209): an asset, a desc with its binary file, a file inside that refers to one outside it, and a level
	// outside that refers to three of them.
	void make_kit()
	{
		write("kit/a.desc", canonical(R"({ "__type": "material_desc" })"));
		write("kit/a.desc.resource", meta_text(k_kit_a_guid));
		write("kit/sub/b.desc", canonical(std::string(R"({ "__type": "level_desc", "a": "res://kit/a.desc",
			"m": { "__type": "mesh_node_desc", "material": { "guid": ")") + std::string(k_kit_a_guid) + R"(", "path": "res://kit/a.desc" } } })"));
		write("kit/sub/b.desc.resource", meta_text(k_kit_b_guid));
		write("kit/sub/rock.mesh.desc", canonical(R"({ "__type": "mesh_desc" })"));
		write("kit/sub/rock.mesh.desc.resource", meta_text(k_kit_rock_guid));
		write("kit/sub/rock.mesh.bin", std::string("SNKB\x01\x00\x00\x00 geometry", 17));
		write("kit/sub/rock.mesh.bin.resource", meta_text(k_kit_rock_bin_guid));
		write("levels/kit_user.desc", canonical(std::string(R"({ "__type": "level_desc", "a": "res://kit/a.desc",
			"rock": "res://kit/sub/rock.mesh.desc",
			"b": { "__type": "mesh_node_desc", "material": { "guid": ")") + std::string(k_kit_b_guid) + R"(", "path": "res://kit/sub/b.desc" } } })"));
	}

	// What a crash leaves: the project's folder, the journals and the history's list as they are now, to be put back by
	// back_to_crash.
	void keep_crash()
	{
		for (const char* part : { "layer", "journal", "settings" }) {
			fs::remove_all(crash.path() / part);
			fs::create_directories(crash.path() / part);
		}
		fs::copy(layer.path(), crash.path() / "layer", fs::copy_options::recursive);
		fs::copy(journal.path(), crash.path() / "journal", fs::copy_options::recursive);
		fs::copy(settings.path(), crash.path() / "settings", fs::copy_options::recursive);
	}

	void back_to_crash()
	{
		const std::pair<const fs::path*, const char*> parts[] = { { &layer.path(), "layer" }, { &journal.path(), "journal" }, { &settings.path(), "settings" } };
		for (const auto& [where, part] : parts) {
			for (const fs::directory_entry& entry : fs::directory_iterator(*where)) fs::remove_all(entry.path());
			fs::copy(crash.path() / part, *where, fs::copy_options::recursive);
		}
	}

	// The string at `pointer` of the JSON file `relative`.
	std::string value_at(std::string_view relative, std::string_view pointer) const
	{
		const json::value document = json::parse(text_of(relative));
		return std::string(document.at_pointer(pointer).as_string().c_str());
	}

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
	test_util::temp_dir lower;
	test_util::temp_dir cache;
	test_util::temp_dir journal;
	test_util::temp_dir foreign;
	ds::task_system tasks{ test_util::manual_tasks() };
	edt::editor_events events;
	edt::job_service jobs{ tasks, &events };
	std::unique_ptr<edt::asset_database> db;
	std::set<std::string> open_files;
	fs::path manifest_dir;
	std::string fail_at;
	std::string throw_at;
	bool throw_in_hold = false;
	bool copy_instead_of_rename = false;
	std::string rename_error_text;
	fs::path journal_override;
	std::function<void(std::string_view)> on_step;
	std::vector<std::string> steps;
	int holds = 0;
	std::vector<res::file_change> self_changes;
	bool forward_self_changes = false;
	edt::asset_ops_limits limits;
	test_util::temp_dir crash{ "asset_ops_crash" };
	test_util::temp_dir settings{ "asset_ops_settings" };
	std::uint64_t history_serial = 0;
	std::unique_ptr<edt::asset_history> history;
	bool throw_when_listed = false;

	// 40 assets with their .resource in big/: an operation of more than 64 steps.
	void make_big()
	{
		for (int i = 0; i < 40; ++i) {
			write(std::format("big/f{:02}.desc", i), canonical(R"({ "__type": "material_desc" })"));
			write(std::format("big/f{:02}.desc.resource", i), meta_text(std::format("12345678-0000-4000-8000-{:012x}", i + 1)));
		}
	}

	// A journal folder written by hand, listed in the history as an operation that did not end.
	void forge_pending(const std::string& id)
	{
		edt::operation_journal listed;
		listed.id = id;
		listed.directory = (journal.path() / id).generic_string();
		BOOST_REQUIRE(history);
		BOOST_REQUIRE(history->record_pending(listed).empty());
	}

	// The history as the editor makes it at a start: the list in `settings`, read again.
	edt::asset_history& start_history()
	{
		history = std::make_unique<edt::asset_history>(journal.path(), settings.path() / "list.json", [this] { return ++history_serial; });
		history->restore(ops().changeable_roots());
		return *history;
	}

	bool announced(res::change_kind kind, std::string_view tag, std::string_view old_tag = {}) const
	{
		return std::ranges::any_of(self_changes, [&](const res::file_change& change) {
			return change.self && change.kind == kind && change.tag.view() == tag && (old_tag.empty() || change.old_tag.view() == old_tag);
		});
	}
};

}

// A delete, an undo that fails (before a rename, or after a copy was checked), the kept file still there, and the same undo again.
static void check_failed_undo(ops_fixture& f, bool copying)
{
	f.open();
	const auto before = f.snapshot();
	f.copy_instead_of_rename = copying;
	const edt::operation_report report = f.run(ops_fixture::request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	const fs::path kept = f.journal.path() / report.operation_id / "files" / "lonely.desc";

	f.fail_at = copying ? "unstash_copied:lonely.desc" : "unstash:lonely.desc";
	const edt::undo_report failed = f.ops().undo(report.journal);
	BOOST_TEST(!failed.ok);
	BOOST_TEST(failed.failures.size() == 1u);
	BOOST_TEST(fs::is_regular_file(kept), "the kept file is the only copy, and it is still there");
	BOOST_TEST(!f.has("lonely.desc"));
	for (const auto& entry : f.snapshot()) {
		BOOST_TEST(entry.first.find(".tmp") == std::string::npos, "no temp file is left: " + entry.first);
	}

	f.fail_at.clear();
	const edt::undo_report again = f.ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(again.ok, again.error);
	BOOST_TEST((f.snapshot() == before));
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
	BOOST_TEST(std::string(components.at("foreign").at("material").at("path").as_string().c_str()) == "res://sky.desc", "another asset's reference stays");
	BOOST_TEST(std::string(components.at("other").at("material").as_string().c_str()) == "res://other.desc");
	BOOST_TEST(std::string(level.at("web").as_string().c_str()) == "https://example.com/sky.desc");
	BOOST_TEST(text_of("levels/main.desc") == canonical(text_of("levels/main.desc")), "still in the project's layout");

	const json::value child = json::parse(text_of("materials/child.desc"));
	BOOST_TEST(std::string(child.at("__parent").as_string().c_str()) == "res://clouds.desc");

	BOOST_REQUIRE(report.rewritten.size() == 2u);
	const edt::rewritten_file* main = rewritten_in(report, "res://levels/main.desc");
	BOOST_REQUIRE(main);
	BOOST_TEST(main->json_pointers.size() == 3u);
	BOOST_TEST(contains(main->json_pointers, k_node + "/sky"));
	BOOST_TEST(contains(main->json_pointers, k_node + "/mesh/material"));
	BOOST_TEST(contains(main->json_pointers, k_node + "/inst/__parent"));
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
	const edt::operation_plan both = ops().plan(two);
	BOOST_TEST(!both.refused.has_value(), "several files are one plan (#1209)");
	BOOST_TEST(both.items.size() == 2u);
	const edt::asset_request renames[] = { request(edt::asset_operation::rename, "sky.desc", "a.desc"), request(edt::asset_operation::rename, "other.desc", "b.desc") };
	BOOST_TEST(((ops().plan(renames).refused->code) == edt::refusal_code::bad_name), "a rename takes one file or folder");
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

BOOST_AUTO_TEST_CASE(AReferrerThatDoesNotWriteBackUnchanged_RefusesTheOperation_AndNothingIsTouched)
{
	write("levels/main.desc", k_level);
	open();
	const auto before = snapshot();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE(report.refused.has_value());
	BOOST_TEST((report.refused->code == edt::refusal_code::unrewritable_references));
	BOOST_REQUIRE(report.refused->unrewritable.size() == 1u);
	BOOST_TEST(report.refused->unrewritable[0].file == "res://levels/main.desc");
	BOOST_TEST(report.refused->unrewritable[0].code == "not_canonical");
	BOOST_TEST(!report.refused->unrewritable[0].reason.empty());
	BOOST_TEST(!report.refused->referrers.empty(), "a reference by path alone no longer finds the asset");
	BOOST_TEST((snapshot() == before), "no partial operation: the other referrer is not rewritten either");
	BOOST_TEST(holds == 0);
}

BOOST_AUTO_TEST_CASE(AnObjectWithAnotherGuidThanTheAssets_IsReported_AndRefusesTheOperation)
{
	write("levels/lost.desc", canonical(R"({ "__type": "level_desc",
		"lost": { "__type": "mesh_node_desc", "material": { "guid": "00000000-0000-4000-8000-000000000009", "path": "res://sky.desc" } } })"));
	open();
	const auto before = snapshot();
	const edt::operation_plan planned = plan(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE(planned.refused.has_value());
	BOOST_TEST((planned.refused->code == edt::refusal_code::unrewritable_references));
	BOOST_REQUIRE(planned.refused->unrewritable.size() == 1u);
	BOOST_TEST(planned.refused->unrewritable[0].file == "res://levels/lost.desc");
	BOOST_TEST(planned.refused->unrewritable[0].code == "guid_mismatch");
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(AReferrerInAnyLowerLayer_IsNotRewritten_AndAPathReferenceThereRefusesTheOperation)
{
	lower.write("refs.desc", canonical(R"({ "__type": "level_desc", "sky": "res://sky.desc" })"));
	open(false, edt::platform_case_insensitive, true);
	const auto before = snapshot();
	const edt::operation_plan planned = plan(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE(planned.refused.has_value());
	BOOST_TEST((planned.refused->code == edt::refusal_code::unrewritable_references));
	BOOST_REQUIRE(planned.refused->unrewritable.size() == 1u);
	BOOST_TEST(planned.refused->unrewritable[0].file == "res://refs.desc");
	BOOST_TEST(planned.refused->unrewritable[0].code == "lower_layer");
	BOOST_TEST(planned.refused->unrewritable[0].reason.find("lower layer") != std::string::npos);
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(text_of("sky.desc") == canonical(R"({ "__type": "material_desc" })"));
}

BOOST_AUTO_TEST_CASE(AReferrerInTheEngineLayer_ByPath_RefusesTheOperation)
{
	engine.write("eng_ref.desc", canonical(R"({ "__type": "level_desc", "sky": "res://sky.desc" })"));
	open();
	const edt::operation_plan planned = plan(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE(planned.refused.has_value());
	BOOST_TEST((planned.refused->code == edt::refusal_code::unrewritable_references));
	BOOST_REQUIRE(planned.refused->unrewritable.size() == 1u);
	BOOST_TEST(planned.refused->unrewritable[0].code == "engine_layer");
}

BOOST_AUTO_TEST_CASE(AReferrerInALowerLayer_ByGuidAlone_StaysValid_AndIsReportedAsNotRewritten)
{
	lower.write("refs.desc", canonical(R"({ "__type": "level_desc", "mat": { "guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f" } })"));
	open(false, edt::platform_case_insensitive, true);
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	const bool reported = std::ranges::any_of(report.warnings, [](const std::string& warning) { return warning.find("lower layer") != std::string::npos; });
	BOOST_TEST(reported);
	BOOST_TEST(rewritten_in(report, "res://refs.desc") == nullptr);
}

BOOST_AUTO_TEST_CASE(ADeleteWithReferrersAndNoForce_IsRefusedWithTheirList)
{
	open();
	const auto before = snapshot();
	const edt::operation_report report = run(request(edt::asset_operation::remove, "sky.desc"));
	BOOST_REQUIRE(report.refused.has_value());
	BOOST_TEST((report.refused->code == edt::refusal_code::has_referrers));
	BOOST_TEST(report.refused->referrers.size() == 4u);
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(AForcedDelete_LeavesTheReferrers_AndReportsThemBroken)
{
	open();
	const std::string level = text_of("levels/main.desc");
	const edt::operation_report report = run(request(edt::asset_operation::remove, "sky.desc"), true);
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(report.broken_references.size() == 4u);
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
	const bool main_reported = std::ranges::find(undone.changed_since, "levels/main.desc") != undone.changed_since.end();
	BOOST_TEST(main_reported);
	BOOST_TEST((snapshot() == after_edit));

	write("clouds.desc", canonical(R"({ "__type": "material_desc", "edited": true })"));
	const edt::undo_report moved_file = ops().undo(report.journal);
	BOOST_TEST(!moved_file.ok);
	const bool clouds_reported = std::ranges::find(moved_file.changed_since, "clouds.desc") != moved_file.changed_since.end();
	BOOST_TEST(clouds_reported);
}

// #1309: a file put at the source between the undo's check and its step is not taken for the deleted one.
BOOST_AUTO_TEST_CASE(AnUndoOfADelete_WithAnotherFileAtItsPlace_FailsSayingWhatToDo_AndKeepsTheDeletedFile)
{
	open();
	const auto before = snapshot();
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	const fs::path kept = journal.path() / report.operation_id / "files" / "lonely.desc";
	on_step = [&](std::string_view step) {
		if (step == "before_undo:lonely.desc") write("lonely.desc", "someone else's file");
	};

	const edt::undo_report failed = ops().undo(report.journal);
	BOOST_TEST(!failed.ok);
	BOOST_REQUIRE(failed.failures.size() == 1u);
	BOOST_TEST(failed.failures.front().find("lonely.desc") != std::string::npos, failed.failures.front());
	BOOST_TEST(failed.failures.front().find("rename or delete that file, then undo again") != std::string::npos,
		failed.failures.front());
	BOOST_TEST(text_of("lonely.desc") == "someone else's file", "the other file is left as it is");
	BOOST_TEST(fs::is_regular_file(kept), "the deleted file is still kept in the journal");

	on_step = {};
	fs::remove(layer.path() / "lonely.desc");
	const edt::undo_report again = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(again.ok, again.error);
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(AnUndoOfADelete_WithTheSameBytesAtItsPlace_CountsTheStepAsDone)
{
	open();
	const std::string original = text_of("lonely.desc");
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	on_step = [&](std::string_view step) {
		if (step == "before_undo:lonely.desc") write("lonely.desc", original);
	};

	const edt::undo_report undone = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST(text_of("lonely.desc") == original);
}

BOOST_AUTO_TEST_CASE(ARollbackOfADelete_WithAnotherFileAtItsPlace_KeepsTheStepAndTheDeletedFileInTheJournal)
{
	open();
	fail_at = "stash:lonely.desc.resource";
	on_step = [&](std::string_view step) {
		if (step == "before_undo:lonely.desc") write("lonely.desc", "someone else's file");
	};
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_TEST(!report.ok);
	BOOST_REQUIRE(report.rollback_failures.size() == 1u);
	const std::string& stuck = report.rollback_failures.front();
	BOOST_TEST(stuck.find("lonely.desc") != std::string::npos, stuck);
	BOOST_TEST(text_of("lonely.desc") == "someone else's file");

	const fs::path folder = journal.path() / report.operation_id;
	BOOST_TEST(fs::is_regular_file(folder / "files" / "lonely.desc"), "the deleted file is still kept in the journal");
	std::ifstream in(folder / "journal.json", std::ios::binary);
	const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	const json::value written = json::parse(text);
	const json::array& left = written.at("steps").as_array();
	BOOST_REQUIRE(left.size() == 1u);
	const json::object& step = left.front().as_object();
	BOOST_TEST(json::value_to<std::string>(step.at("kind")) == "stash");
	BOOST_TEST(json::value_to<std::string>(step.at("from")) == "lonely.desc");
	BOOST_TEST(json::value_to<std::string>(step.at("to")) == "files/lonely.desc", "the step still names the kept file");
}

BOOST_AUTO_TEST_CASE(AnUndoOfARename_WithAnotherFileAtTheOldName_FailsAndLeavesTheMovedFile)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	const std::string moved = text_of("clouds.desc");
	on_step = [&](std::string_view step) {
		if (step == "before_undo:sky.desc") write("sky.desc", "someone else's file");
	};

	const edt::undo_report failed = ops().undo(report.journal);
	BOOST_TEST(!failed.ok);
	const bool named = std::ranges::any_of(failed.failures, [](const std::string& failure) {
		return failure.find("sky.desc") != std::string::npos
			&& failure.find("rename or delete that file, then undo again") != std::string::npos;
	});
	BOOST_TEST(named);
	BOOST_TEST(text_of("sky.desc") == "someone else's file");
	BOOST_TEST(text_of("clouds.desc") == moved, "the moved file stays where the rename put it");
}

BOOST_AUTO_TEST_CASE(AnUndoOfARename_WhoseOldNameIsBackWhileTheNewOneIsStillThere_Fails_AndRemovesNeither)
{
	open();
	const std::string original = text_of("sky.desc");
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	on_step = [&](std::string_view step) {
		if (step == "before_undo:sky.desc") write("sky.desc", original);
	};

	const edt::undo_report failed = ops().undo(report.journal);
	BOOST_TEST(!failed.ok);
	const bool named = std::ranges::any_of(failed.failures, [](const std::string& failure) {
		return failure.find("clouds.desc") != std::string::npos && failure.find("then undo again") != std::string::npos;
	});
	BOOST_TEST(named);
	BOOST_TEST(text_of("sky.desc") == original);
	BOOST_TEST(has("clouds.desc"), "the file at the new name is not taken away");
}

BOOST_AUTO_TEST_CASE(ADeleteOrAMoveOfAFileOpenWithUnsavedChanges_IsRefused_AsIsARename)
{
	open();
	const auto before = snapshot();
	open_files = { "res://lonely.desc" };
	for (const edt::asset_operation operation : { edt::asset_operation::remove, edt::asset_operation::move, edt::asset_operation::rename }) {
		const edt::operation_report report = run(request(operation, "lonely.desc", operation == edt::asset_operation::move ? "env" : "x.desc"), true);
		BOOST_REQUIRE_MESSAGE(report.refused.has_value(), edt::asset_operation_name(operation));
		BOOST_TEST((report.refused->code == edt::refusal_code::referrer_open), edt::asset_operation_name(operation));
	}
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(holds == 0);
}

BOOST_AUTO_TEST_CASE(AFileThatTurnsDirtyBetweenThePlanAndTheExecute_IsRefusedAtTheExecute)
{
	open();
	edt::asset_ops operations = ops();
	const edt::asset_request asked = request(edt::asset_operation::remove, "lonely.desc");
	const edt::operation_plan planned = operations.plan(std::span<const edt::asset_request>(&asked, 1));
	BOOST_REQUIRE(!planned.refused.has_value());
	open_files = { "res://lonely.desc" };
	const edt::operation_report report = operations.execute(planned);
	BOOST_REQUIRE(report.refused.has_value());
	BOOST_TEST((report.refused->code == edt::refusal_code::referrer_open));
	BOOST_TEST(has("lonely.desc"));
}

BOOST_AUTO_TEST_CASE(AJournalWithAForeignRoot_IsRefused_AndNothingIsTouched)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	const fs::path victim = foreign.write("clouds.desc", "not the project's");
	const auto after = snapshot();

	edt::operation_journal bad = report.journal;
	for (edt::journal_step& step : bad.steps) step.root = foreign.path().generic_string();
	const edt::undo_report undone = ops().undo(bad);
	BOOST_TEST(!undone.ok);
	BOOST_TEST(!undone.error.empty());
	BOOST_TEST(undone.restored.empty());
	BOOST_TEST((snapshot() == after));
	BOOST_TEST(fs::is_regular_file(victim));
}

BOOST_AUTO_TEST_CASE(AJournalWithAFolderThatIsNotItsOwn_OrAnUnsafeId_IsRefused_AndNothingIsTouched)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	const auto after = snapshot();

	edt::operation_journal elsewhere = report.journal;
	elsewhere.directory = (foreign.path() / report.journal.id).generic_string();
	const edt::undo_report folder = ops().undo(elsewhere);
	BOOST_TEST(!folder.ok);
	BOOST_TEST(!folder.error.empty());

	for (const std::string_view id : { "../evil", "a/b", "", "..", "x y" }) {
		edt::operation_journal other = report.journal;
		other.id = std::string(id);
		other.directory = (journal.path() / other.id).generic_string();
		const edt::undo_report refused = ops().undo(other);
		BOOST_TEST(!refused.ok, id);
		BOOST_TEST(refused.restored.empty(), id);
	}
	BOOST_TEST((snapshot() == after));
}

BOOST_AUTO_TEST_CASE(AJournalWithAnAbsoluteOrDotDotPath_IsRefusedWhenRead_AndWhenUndone)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	const auto after = snapshot();
	const std::string unsafe_paths[] = { "../outside.desc", "a/../../outside.desc", "/etc/passwd", "a\\b", "C:/x", "a//b", "" };
	for (const std::string& path : unsafe_paths) {
		for (const bool in_from : { true, false }) {
			edt::operation_journal bad = report.journal;
			BOOST_REQUIRE(!bad.steps.empty());
			(in_from ? bad.steps.front().from : bad.steps.front().to) = path;
			BOOST_TEST(!edt::journal_problem(bad).empty(), path);
			std::string reason;
			const std::optional<edt::operation_journal> read = edt::journal_from_json(edt::journal_to_json(bad), reason);
			BOOST_TEST(!read.has_value(), path);
			BOOST_TEST(!reason.empty(), path);
			const edt::undo_report undone = ops().undo(bad);
			BOOST_TEST(!undone.ok, path);
			BOOST_TEST(undone.restored.empty(), path);
		}
	}
	BOOST_TEST((snapshot() == after));

	edt::operation_journal original_path = report.journal;
	const auto rewrite = std::ranges::find(original_path.steps, edt::journal_step_kind::rewrite, &edt::journal_step::kind);
	BOOST_REQUIRE(rewrite != original_path.steps.end());
	rewrite->original = "../../secret";
	BOOST_TEST(!edt::journal_problem(original_path).empty());
	BOOST_TEST(!ops().undo(original_path).ok);
	BOOST_TEST((snapshot() == after));
	BOOST_TEST(edt::journal_problem(report.journal).empty(), "the journal of an operation is fine");
}

BOOST_AUTO_TEST_CASE(AJournalStepOnANameTheProjectDoesNotList_OrARewriteOfNoDescOrManifest_IsRefused)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(edt::journal_problem(report.journal).empty());

	for (const std::string_view name : { ".git/config", ".hidden.desc", "a/.tmp/x.desc" }) {
		for (const bool in_from : { true, false }) {
			edt::operation_journal bad = report.journal;
			(in_from ? bad.steps.front().from : bad.steps.front().to) = std::string(name);
			BOOST_TEST(!edt::journal_problem(bad).empty(), name);
			BOOST_TEST(!ops().undo(bad).ok, name);
		}
	}

	edt::operation_journal rewrite_of_a_text = report.journal;
	const auto rewrite = std::ranges::find(rewrite_of_a_text.steps, edt::journal_step_kind::rewrite, &edt::journal_step::kind);
	BOOST_REQUIRE(rewrite != rewrite_of_a_text.steps.end());
	rewrite->from = "levels/notes.txt";
	BOOST_TEST(!edt::journal_problem(rewrite_of_a_text).empty(), "a rewrite touches descs and the manifest only");
	rewrite->from = std::string(prj::k_manifest_file_name);
	BOOST_TEST(edt::journal_problem(rewrite_of_a_text).empty(), "the manifest may be rewritten");

	const edt::operation_report removal = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE_MESSAGE(removal.ok, removal.error);
	edt::operation_journal stash = removal.journal;
	BOOST_REQUIRE(!stash.steps.empty());
	stash.steps.front().to = "files/other.desc";
	BOOST_TEST(!edt::journal_problem(stash).empty(), "a removed file is kept where its own path says");
}

BOOST_AUTO_TEST_CASE(AJournalWithTheRootOfALowerOrTheEngineLayer_IsRefused_ThoughItIsALayerOfTheProject)
{
	open(false, edt::platform_case_insensitive, true);
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	const auto after = snapshot();
	for (const fs::path& foreign_layer : { lower.path(), engine.path() }) {
		edt::operation_journal bad = report.journal;
		for (edt::journal_step& step : bad.steps) step.root = foreign_layer.generic_string();
		const edt::undo_report undone = ops().undo(bad);
		BOOST_TEST(!undone.ok);
		BOOST_TEST(undone.restored.empty());
	}
	BOOST_TEST((snapshot() == after));
}

BOOST_AUTO_TEST_CASE(AJournalWhoseRootIsAnAliasOfTheProjectsFolder_IsRefused_AndNoForgedStepWritesIntoItsOwnFolders)
{
	write(std::string(prj::k_manifest_file_name), "{}");
	open();
	const edt::operation_report removal = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE_MESSAGE(removal.ok, removal.error);
	BOOST_REQUIRE(!removal.journal.steps.empty());
	const std::string canonical = removal.journal.steps.front().root;
	const std::string id = removal.journal.id;
	journal.write(id + "/files/code/evil.cpp", "evil");
	const auto after = snapshot();

	std::vector<std::string> roots{ canonical + "/nosuch/..", canonical + "/nosuch/../", canonical + "/", canonical + "/./", canonical + "//",
		canonical + "/env/..", canonical + "/../" + layer.path().filename().generic_string(), canonical, canonical + "\\" };
	std::string upper = canonical;
	std::ranges::transform(upper, upper.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
	roots.push_back(upper);
	std::error_code link_error;
	fs::create_directory_symlink(layer.path(), foreign.path() / "alias", link_error);
	if (!link_error) roots.push_back((foreign.path() / "alias").generic_string());

	for (const std::string& root : roots) {
		edt::operation_journal forged = removal.journal;
		forged.steps.clear();
		forged.steps.push_back(edt::journal_step{ edt::journal_step_kind::stash, root, "code/evil.cpp", "files/code/evil.cpp", {}, {}, {} });
		const edt::undo_report undone = ops().undo(forged);
		BOOST_TEST(!undone.ok, root);
		BOOST_TEST(!undone.error.empty(), root);
		BOOST_TEST(undone.restored.empty(), root);
		BOOST_TEST(!has("code/evil.cpp"), root);
		BOOST_TEST(!has("build/evil.cpp"), root);
		BOOST_TEST((snapshot() == after), root);
		if (root != canonical && root != upper && !(!link_error && root == (foreign.path() / "alias").generic_string())) {
			BOOST_TEST(!edt::journal_problem(forged).empty(), root);
			std::string reason;
			BOOST_TEST(!edt::journal_from_json(edt::journal_to_json(forged), reason).has_value(), root);
			BOOST_TEST(!reason.empty(), root);
		}
	}

	edt::operation_journal in_second = removal.journal;
	in_second.steps.push_back(edt::journal_step{ edt::journal_step_kind::stash, canonical + "/nosuch/..", "code/evil.cpp", "files/code/evil.cpp", {}, {}, {} });
	BOOST_TEST(!ops().undo(in_second).ok, "every step's root is checked, not the first alone");
	BOOST_TEST(!has("code/evil.cpp"));
	BOOST_TEST((snapshot() == after));
}

BOOST_AUTO_TEST_CASE(TheManifestsFolder_IsOpenToARewriteOfTheManifestAlone)
{
	open();
	manifest_dir = foreign.path();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	const auto after = snapshot();
	const fs::path victim = foreign.write("levels/main.desc", "not the project's");
	for (const edt::journal_step_kind kind : { edt::journal_step_kind::move, edt::journal_step_kind::rewrite }) {
		edt::operation_journal bad = report.journal;
		const auto step = std::ranges::find(bad.steps, kind, &edt::journal_step::kind);
		BOOST_REQUIRE(step != bad.steps.end());
		step->root = foreign.path().generic_string();
		const edt::undo_report undone = ops().undo(bad);
		BOOST_TEST(!undone.ok);
		BOOST_TEST(undone.restored.empty());
	}
	BOOST_TEST((snapshot() == after));
	BOOST_TEST(fs::is_regular_file(victim));
}

BOOST_AUTO_TEST_CASE(AnUndoAfterACrashBetweenSteps_PutsBackWhatHappened_AndSkipsTheStepThatDidNot)
{
	open();
	const auto before = snapshot();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_REQUIRE(report.journal.steps.size() == 4u);
	BOOST_REQUIRE(report.journal.steps.back().kind == edt::journal_step_kind::rewrite);

	write(report.journal.steps.back().from, before.at(report.journal.steps.back().from));
	const edt::undo_report undone = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST(undone.restored.size() == 3u, "the step that did not happen is not reported as restored");
	BOOST_TEST((snapshot() == before));

	const edt::undo_report again = ops().undo(report.journal);
	BOOST_TEST(again.ok, "undoing a journal whose steps are all as before is not a failure: " + again.error);
	BOOST_TEST(again.restored.empty());
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(AnUndoOfAStepRecordedBeforeAnyActionWasDone_AndOfADeleteThatCopiedButNeverRemoved_Works)
{
	open();
	const auto before = snapshot();
	const edt::operation_report removal = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE_MESSAGE(removal.ok, removal.error);
	BOOST_REQUIRE(removal.journal.steps.size() == 2u);
	for (const edt::journal_step& step : removal.journal.steps) write(step.from, before.at(step.from));
	const edt::undo_report undone = ops().undo(removal.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST(undone.restored.empty());
	BOOST_TEST((snapshot() == before));

	const edt::operation_report moved = run(request(edt::asset_operation::move, "lonely.desc", "env"));
	BOOST_REQUIRE_MESSAGE(moved.ok, moved.error);
	fs::rename(layer.path() / "env/lonely.desc.resource", layer.path() / "lonely.desc.resource");
	fs::rename(layer.path() / "env/lonely.desc", layer.path() / "lonely.desc");
	const edt::undo_report nothing_done = ops().undo(moved.journal);
	BOOST_REQUIRE_MESSAGE(nothing_done.ok, nothing_done.error);
	BOOST_TEST(nothing_done.restored.empty());
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(AnUndoOfAFileThatIsOpenWithUnsavedChanges_IsRefused_AndTouchesNothing)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	const auto after = snapshot();
	open_files = { "res://clouds.desc" };
	const edt::undo_report undone = ops().undo(report.journal);
	BOOST_TEST(!undone.ok);
	BOOST_TEST(!undone.open_with_unsaved_changes.empty());
	BOOST_TEST((snapshot() == after));
	open_files.clear();
	BOOST_TEST(ops().undo(report.journal).ok);
}

BOOST_AUTO_TEST_CASE(AFolderThatIsNotAProjectName_OrIsAFile_IsRefusedWithoutTouchingTheDisk)
{
	open();
	for (const std::string_view folder : { "../outside", "a/../../b", "env/../..", "levels/main.desc" }) {
		const edt::operation_plan planned = plan(request(edt::asset_operation::move, "sky.desc", folder));
		BOOST_REQUIRE_MESSAGE(planned.refused.has_value(), folder);
		BOOST_TEST((planned.refused->code == edt::refusal_code::unportable_path || planned.refused->code == edt::refusal_code::bad_name
			|| planned.refused->code == edt::refusal_code::no_such_folder), folder);
	}
	const edt::operation_plan file_as_folder = plan(request(edt::asset_operation::move, "sky.desc", "levels/main.desc"));
	BOOST_REQUIRE(file_as_folder.refused.has_value());
	BOOST_TEST((file_as_folder.refused->code == edt::refusal_code::no_such_folder));
}

BOOST_AUTO_TEST_CASE(AnOrphanResourceThatDiffersInCaseOnly_Refuses_WhereNamesIgnoreCase)
{
	open(false, true);
	write("Clouds.desc.resource", meta_text(k_lonely_guid));
	const edt::operation_plan planned = plan(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE(planned.refused.has_value());
	BOOST_TEST((planned.refused->code == edt::refusal_code::target_exists));
}

BOOST_AUTO_TEST_CASE(APortThatThrows_RollsEverythingBack_AndTheReportSaysWhy)
{
	open();
	const auto before = snapshot();
	for (const std::string_view step : { "move:sky.desc", "move:sky.desc.resource", "rewrite:levels/main.desc", "rewrite:materials/child.desc" }) {
		throw_at = std::string(step);
		edt::operation_report report;
		BOOST_REQUIRE_NO_THROW(report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc")));
		BOOST_TEST(!report.ok, step);
		BOOST_TEST(!report.refused.has_value(), step);
		BOOST_TEST(report.error == "port threw", step);
		BOOST_TEST(report.rollback_failures.empty(), step);
		BOOST_TEST((snapshot() == before), std::string("everything is as it was after a throw at ") + std::string(step));
	}
	throw_at = "stash:sky.desc.resource";
	edt::operation_report removal;
	BOOST_REQUIRE_NO_THROW(removal = run(request(edt::asset_operation::remove, "sky.desc"), true));
	BOOST_TEST(!removal.ok);
	BOOST_TEST((snapshot() == before));

	throw_at.clear();
	throw_in_hold = true;
	edt::operation_report held;
	BOOST_REQUIRE_NO_THROW(held = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc")));
	BOOST_TEST(!held.ok);
	BOOST_TEST(held.error == "hold refused");
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(EveryStepIsInTheJournalOnTheDisk_BeforeItsActionIsDone)
{
	open();
	std::size_t journaled_at_rewrite = 0;
	on_step = [&](std::string_view step) {
		if (step != "rewrite:levels/main.desc") return;
		for (const fs::directory_entry& entry : fs::directory_iterator(journal.path())) {
			std::ifstream file(entry.path() / "journal.json", std::ios::binary);
			const json::value document = json::parse(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
			journaled_at_rewrite = document.at("steps").as_array().size();
		}
	};
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(journaled_at_rewrite == 2u, "the two moves were on the disk before the first rewrite began");

	const json::value last = json::parse([&] {
		std::ifstream file(journal.path() / report.operation_id / "journal.json", std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}());
	BOOST_TEST(last.at("steps").as_array().size() == 4u);
}

BOOST_AUTO_TEST_CASE(ADeleteAndItsUndo_MoveTheFileByRename_NoSecondCopyIsMade)
{
	open();
	const auto before = snapshot();
	const std::string bytes = text_of("lonely.desc");
	const auto identity = identity_of(layer.path() / "lonely.desc");
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);

	const fs::path kept = journal.path() / report.operation_id / "files" / "lonely.desc";
	BOOST_TEST(!has("lonely.desc"));
	BOOST_REQUIRE(fs::is_regular_file(kept));
	BOOST_TEST(fs::file_size(kept) == bytes.size());
	const auto kept_identity = identity_of(kept);
	if (identity.first != 0u && kept_identity.second == identity.second) {
		BOOST_TEST(kept_identity.first == identity.first, "the file itself is in the journal, not a copy of it");
	}

	const edt::undo_report undone = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(!fs::exists(kept), "the journal holds no second copy after the undo");
	if (identity.first != 0u && kept_identity.second == identity.second) {
		BOOST_TEST(identity_of(layer.path() / "lonely.desc").first == identity.first);
	}
}

BOOST_AUTO_TEST_CASE(WhereARenameCannotBeMade_ADeleteAndItsUndo_CopyFlushAndCheckTheFile)
{
	open();
	const auto before = snapshot();
	copy_instead_of_rename = true;
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(!has("lonely.desc"));
	const fs::path kept = journal.path() / report.operation_id / "files" / "lonely.desc";
	BOOST_REQUIRE(fs::is_regular_file(kept));
	BOOST_TEST((fs::file_size(kept) == before.at("lonely.desc").size()));

	const edt::undo_report undone = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST((snapshot() == before), "no temp file is left beside the restored file");
	BOOST_TEST(!fs::exists(kept), "the kept copy goes once the restored file is whole");
}

BOOST_AUTO_TEST_CASE(ACopyThatFailsBeforeItIsInPlace_LeavesTheOriginal_AndTheDeleteIsTakenBack)
{
	open();
	const auto before = snapshot();
	copy_instead_of_rename = true;
	fail_at = "stash_copied:lonely.desc";
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_TEST(!report.ok);
	BOOST_TEST(report.error.find("injected failure") != std::string::npos);
	BOOST_TEST(report.rollback_failures.empty());
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(!fs::exists(journal.path() / report.operation_id / "files" / "lonely.desc"), "a partial copy is never left at the kept name");
}

BOOST_AUTO_TEST_CASE(AnUndoThatFailsBeforeAMove_LeavesTheKeptFile_AndCanBeTriedAgain)
{
	check_failed_undo(*this, false);
}

BOOST_AUTO_TEST_CASE(AnUndoThatFailsAfterTheCopyWasChecked_LeavesTheKeptFile_AndCanBeTriedAgain)
{
	check_failed_undo(*this, true);
}

BOOST_AUTO_TEST_CASE(ADeleteWhoseSourceIsThereAgainBesideItsKeptCopy_IsFinishedByTheUndo_OrRefusedWhenTheSourceDiffers)
{
	open();
	const auto before = snapshot();
	copy_instead_of_rename = true;
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_REQUIRE(fs::is_regular_file(journal.path() / report.operation_id / "files" / "lonely.desc"));

	write("lonely.desc", before.at("lonely.desc"));
	write("lonely.desc.resource", before.at("lonely.desc.resource"));
	const edt::undo_report finished = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(finished.ok, finished.error);
	BOOST_TEST(finished.restored.empty());
	BOOST_TEST((snapshot() == before));

	write("lonely.desc", "somebody else's edit");
	const edt::undo_report refused = ops().undo(report.journal);
	BOOST_TEST(!refused.ok);
	BOOST_TEST(refused.changed_since.size() == 1u);
	BOOST_TEST(text_of("lonely.desc") == "somebody else's edit");
}

BOOST_AUTO_TEST_CASE(AMoveStepWhoseSourceAndTargetAreBothThere_IsRefusedNotGuessed)
{
	open();
	const auto before = snapshot();
	const edt::operation_report moved = run(request(edt::asset_operation::move, "lonely.desc", "env"));
	BOOST_REQUIRE_MESSAGE(moved.ok, moved.error);
	write("lonely.desc", before.at("lonely.desc"));
	write("lonely.desc.resource", before.at("lonely.desc.resource"));

	const edt::undo_report refused = ops().undo(moved.journal);
	BOOST_TEST(!refused.ok);
	BOOST_TEST(!refused.changed_since.empty());
}

BOOST_AUTO_TEST_CASE(AJournalFolderThatIsALink_RefusesTheDelete_AndNothingGoesThroughIt)
{
	open();
	const auto before = snapshot();
	test_util::temp_dir target("asset_ops_link_target");
	const fs::path link = journal.path().parent_path() / "asset_ops_journal_link";
	std::error_code ec;
	fs::remove(link, ec);
	ec.clear();
#ifdef _WIN32
	// A junction needs no privilege: one that cannot be made is a broken test, not a skip.
	const std::string command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.path().string() + "\" >nul";
	const bool linked = std::system(command.c_str()) == 0 && fs::exists(link);
	BOOST_REQUIRE_MESSAGE(linked, "mklink /J failed at " << link.string());
#else
	fs::create_directory_symlink(target.path(), link, ec);
	BOOST_REQUIRE_MESSAGE(!ec, "no symlink: " << ec.message());
#endif
	journal_override = link;
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	fs::remove(link, ec);
	BOOST_TEST(!report.ok);
	BOOST_TEST((snapshot() == before), "the original file is where it was");
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(target.path())) {
		BOOST_TEST(entry.path().filename().string().find("lonely") == std::string::npos, "nothing of the asset went through the link: " + entry.path().string());
	}
}

BOOST_AUTO_TEST_CASE(ARenameThatFailsForAnyReasonButAnotherVolume_RefusesTheStep_AndCopiesNothing)
{
	open();
	const auto before = snapshot();
	rename_error_text = "access denied";
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_TEST(!report.ok);
	BOOST_TEST(report.error.find("access denied") != std::string::npos);
	BOOST_TEST(report.rollback_failures.empty());
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(!fs::exists(journal.path() / report.operation_id / "files" / "lonely.desc"), "no copy was made instead");
}

BOOST_AUTO_TEST_CASE(AnUndoWhoseRenameFailsForAnotherReasonThanTheVolume_FailsTheStep_AndKeepsTheKeptFile)
{
	open();
	const auto before = snapshot();
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	const fs::path kept = journal.path() / report.operation_id / "files" / "lonely.desc";

	rename_error_text = "access denied";
	const edt::undo_report failed = ops().undo(report.journal);
	BOOST_TEST(!failed.ok);
	BOOST_TEST(!failed.failures.empty());
	BOOST_TEST(fs::is_regular_file(kept));
	BOOST_TEST(!has("lonely.desc"), "nothing was copied back");

	rename_error_text.clear();
	const edt::undo_report again = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(again.ok, again.error);
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(AKeptFileThatChangesJustBeforeItComesBack_IsMovedBackIntoTheJournal_AndTheStepFails)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	const fs::path kept = journal.path() / report.operation_id / "files" / "lonely.desc";
	on_step = [&](std::string_view step) {
		if (step == "unstash:lonely.desc") std::ofstream(kept, std::ios::binary | std::ios::trunc) << "corrupted";
	};

	const edt::undo_report failed = ops().undo(report.journal);
	BOOST_TEST(!failed.ok);
	BOOST_TEST(failed.failures.size() == 1u);
	BOOST_TEST(!has("lonely.desc"), "a file that is not the removed one is not left in the project");
	BOOST_REQUIRE(fs::is_regular_file(kept));
	std::ifstream in(kept, std::ios::binary);
	BOOST_TEST((std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) == "corrupted"));
}

BOOST_AUTO_TEST_CASE(ADeleteOfADescWithItsBinaryFile_StashesBothInChunks_AndUndoPutsThemBack)
{
	open();
	const auto before = snapshot();
	const edt::operation_report report = run(request(edt::asset_operation::remove, "meshes/rock.mesh.desc"), true);
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(!has("meshes/rock.mesh.desc"));
	BOOST_TEST(!has("meshes/rock.mesh.bin"));
	BOOST_TEST(!has("meshes/rock.mesh.bin.resource"));
	BOOST_TEST(fs::is_regular_file(journal.path() / report.operation_id / "files" / "meshes" / "rock.mesh.bin"));

	const edt::undo_report undone = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(ARenameMovesTheBinaryFileBeforeTheDesc_AndADeleteRemovesTheDescFirst)
{
	open();
	run(request(edt::asset_operation::rename, "meshes/rock.mesh.desc", "stone.mesh.desc"));
	const std::vector<std::string> rename_order{ "move:meshes/rock.mesh.bin", "move:meshes/rock.mesh.bin.resource", "move:meshes/rock.mesh.desc",
		"move:meshes/rock.mesh.desc.resource", "rewrite:levels/rocks.desc" };
	BOOST_TEST((steps == rename_order));

	steps.clear();
	open();
	run(request(edt::asset_operation::remove, "meshes/stone.mesh.desc"), true);
	const std::vector<std::string> delete_order{ "stash:meshes/stone.mesh.desc", "stash:meshes/stone.mesh.desc.resource", "stash:meshes/stone.mesh.bin",
		"stash:meshes/stone.mesh.bin.resource" };
	BOOST_TEST((steps == delete_order));
}

BOOST_AUTO_TEST_CASE(ADestinationThatTheProjectDoesNotList_IsRefused)
{
	open();
	for (const std::string_view name : { ".hidden.desc", "x.tmp", "Thumbs.db", "x.desc.resource", "x.mesh.bin", "x.mesh.bin.new", "~$x.desc" }) {
		const edt::operation_plan planned = plan(request(edt::asset_operation::rename, "lonely.desc", name));
		BOOST_REQUIRE_MESSAGE(planned.refused.has_value(), name);
		BOOST_TEST((planned.refused->code == edt::refusal_code::bad_name), name);
		BOOST_TEST(!planned.refused->reason.empty(), name);
	}
	write(".git/.keep", "x");
	write(".cache/.keep", "x");
	for (const std::string_view folder : { ".git", ".cache" }) {
		const edt::operation_plan planned = plan(request(edt::asset_operation::move, "lonely.desc", folder));
		BOOST_REQUIRE_MESSAGE(planned.refused.has_value(), folder);
		BOOST_TEST((planned.refused->code == edt::refusal_code::bad_name), folder);
	}
	BOOST_TEST(has("lonely.desc"));
}

BOOST_AUTO_TEST_CASE(AnOrphanResourceOrBinaryFileAtTheDestination_Refuses_EvenWhenTheSourceHasNone)
{
	write("meshes/lonely2.mesh.desc", canonical(R"({ "__type": "mesh_desc" })"));
	open();
	const std::pair<std::string_view, std::string_view> orphans[] = {
		{ "clouds.desc.resource", "clouds.desc" },
		{ "meshes/ghost.mesh.bin", "ghost.mesh.desc" },
		{ "meshes/ghost.mesh.bin.resource", "ghost.mesh.desc" },
		{ "meshes/ghost.mesh.bin.new", "ghost.mesh.desc" },
	};
	for (const auto& [orphan, name] : orphans) {
		write(orphan, "stray");
		const std::string asset = orphan.starts_with("meshes/") ? "meshes/lonely2.mesh.desc" : "lonely.desc";
		const edt::operation_plan planned = plan(request(edt::asset_operation::rename, asset, name));
		BOOST_REQUIRE_MESSAGE(planned.refused.has_value(), orphan);
		BOOST_TEST((planned.refused->code == edt::refusal_code::target_exists), orphan);
		fs::remove(layer.path() / std::string(orphan));
	}
}

BOOST_AUTO_TEST_CASE(ABinaryFileInALowerLayer_RefusesTheOperation_WithItsReason)
{
	engine.write("meshes/pebble.mesh.bin", "SNKB");
	write("meshes/pebble.mesh.desc", canonical(R"({ "__type": "mesh_desc" })"));
	open();
	for (const edt::asset_operation operation : { edt::asset_operation::rename, edt::asset_operation::remove }) {
		const edt::operation_plan planned = plan(request(operation, "meshes/pebble.mesh.desc", "sand.mesh.desc"));
		BOOST_REQUIRE_MESSAGE(planned.refused.has_value(), edt::asset_operation_name(operation));
		BOOST_TEST((planned.refused->code == edt::refusal_code::binary_pair_blocked), edt::asset_operation_name(operation));
		BOOST_TEST(planned.refused->reason.find("lower layer") != std::string::npos);
	}
	BOOST_TEST(has("meshes/pebble.mesh.desc"));
}

BOOST_AUTO_TEST_CASE(AStagedBinaryFile_RefusesTheOperation)
{
	write("meshes/rock.mesh.bin.new", "half written");
	open();
	const edt::operation_plan planned = plan(request(edt::asset_operation::rename, "meshes/rock.mesh.desc", "stone.mesh.desc"));
	BOOST_REQUIRE(planned.refused.has_value());
	BOOST_TEST((planned.refused->code == edt::refusal_code::binary_pair_blocked));
	BOOST_TEST(has("meshes/rock.mesh.bin"));
}

BOOST_AUTO_TEST_CASE(ABinaryFileThatSomethingElseRefersTo_RefusesTheOperation_WithTheReferrers)
{
	write("levels/blob_user.desc", canonical(R"({ "__type": "level_desc", "blob": "res://meshes/rock.mesh.bin" })"));
	open();
	const edt::operation_plan planned = plan(request(edt::asset_operation::remove, "meshes/rock.mesh.desc"));
	BOOST_REQUIRE(planned.refused.has_value());
	BOOST_TEST((planned.refused->code == edt::refusal_code::binary_pair_blocked));
	BOOST_REQUIRE(planned.refused->referrers.size() == 1u);
	BOOST_TEST(planned.refused->referrers[0].file == "res://levels/blob_user.desc");
}

BOOST_AUTO_TEST_CASE(AReferrerSavedBetweenThePlanAndTheReplace_IsNotOverwritten)
{
	open();
	const std::string edited = canonical(R"({ "__type": "level_desc", "edited": true, "sky": "res://sky.desc" })");
	on_step = [&](std::string_view step) {
		if (step == "rewrite:levels/main.desc") write("levels/main.desc", edited);
	};
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_TEST(!report.ok);
	BOOST_TEST(report.error.find("changed meanwhile") != std::string::npos);
	BOOST_TEST(text_of("levels/main.desc") == edited, "the file saved meanwhile is what stays");
	BOOST_TEST(has("sky.desc"), "the moves were taken back");
	BOOST_TEST(!has("clouds.desc"));
	BOOST_TEST(report.rollback_failures.empty());
}

BOOST_AUTO_TEST_CASE(AReferrerSavedAfterItWasRewritten_IsNotOverwrittenByTheRollback)
{
	open();
	const std::string edited = canonical(R"({ "__type": "level_desc", "edited": true })");
	fail_at = "rewrite:materials/child.desc";
	on_step = [&](std::string_view step) {
		if (step == "rewrite:materials/child.desc") write("levels/main.desc", edited);
	};
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_TEST(!report.ok);
	BOOST_TEST(text_of("levels/main.desc") == edited, "the rollback leaves what was saved after the rewrite");
	BOOST_REQUIRE(!report.rollback_failures.empty());
	BOOST_TEST(report.rollback_failures.front().find("levels/main.desc") != std::string::npos);
	BOOST_TEST(has("sky.desc"), "the moves, which nobody touched, were taken back");
}

BOOST_AUTO_TEST_CASE(AnEngineLayerHelper_NamesTheLayerThatIsTheEngines)
{
	const std::vector<fs::path> layers{ engine.path(), layer.path() };
	BOOST_TEST(edt::is_engine_layer(layers, 0, engine.path()));
	BOOST_TEST(!edt::is_engine_layer(layers, 1, engine.path()));
	BOOST_TEST(!edt::is_engine_layer(layers, 0, {}), "no engine layer configured");
	BOOST_TEST(!edt::is_engine_layer(layers, 5, engine.path()));
}

BOOST_AUTO_TEST_CASE(ARename_AnnouncesItsFilesAsTheEditorsOwnChange)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(announced(res::change_kind::renamed, "res://clouds.desc", "res://sky.desc"));
	BOOST_TEST(announced(res::change_kind::renamed, "res://clouds.desc.resource", "res://sky.desc.resource"));
	BOOST_TEST(announced(res::change_kind::modified, "res://levels/main.desc"));
	BOOST_TEST(announced(res::change_kind::modified, "res://materials/child.desc"));
	BOOST_TEST(self_changes.size() == 4u);
}

BOOST_AUTO_TEST_CASE(ADeleteAndItsUndo_AreAnnouncedAsADeleteAndACreate)
{
	open();
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(announced(res::change_kind::deleted, "res://lonely.desc"));
	BOOST_TEST(announced(res::change_kind::deleted, "res://lonely.desc.resource"));
	self_changes.clear();
	const edt::undo_report undone = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST(announced(res::change_kind::created, "res://lonely.desc"));
	BOOST_TEST(announced(res::change_kind::created, "res://lonely.desc.resource"));
}

BOOST_AUTO_TEST_CASE(AFailureThatWasTakenBackAnnouncesNothing)
{
	open();
	fail_at = "rewrite:levels/main.desc";
	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE(!report.ok);
	BOOST_TEST(self_changes.empty(), "the disk is as it was, so the database has nothing to hear");
}

BOOST_AUTO_TEST_CASE(TheDatabaseTakesAnAnnouncedMoveAsOne_AndTheWatchersLaterReportAddsNothing)
{
	edt::asset_database& database = open();
	std::vector<edt::asset_batch> batches;
	database.set_on_batch([&batches](const edt::asset_batch& batch) { batches.push_back(batch); });
	forward_self_changes = true;

	const edt::operation_report report = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(database.find(res::tag::make("clouds.desc")) != nullptr);
	BOOST_TEST(database.find(res::tag::make("sky.desc")) == nullptr);
	BOOST_REQUIRE(batches.size() == 1u);
	BOOST_REQUIRE(batches[0].moved.size() == 1u);
	BOOST_TEST(batches[0].moved[0].first.view() == "res://sky.desc");
	BOOST_TEST(batches[0].moved[0].second.view() == "res://clouds.desc");
	BOOST_TEST(batches[0].added.empty());
	BOOST_TEST(batches[0].removed.empty());

	// What the watcher reports a poll later, as somebody else's delete and create.
	std::vector<res::file_change> watcher;
	for (const std::string_view name : { "sky.desc", "sky.desc.resource" }) {
		watcher.push_back(res::file_change{ res::tag::make(name), res::tag{}, res::change_kind::deleted, false, false });
	}
	for (const std::string_view name : { "clouds.desc", "clouds.desc.resource" }) {
		watcher.push_back(res::file_change{ res::tag::make(name), res::tag{}, res::change_kind::created, false, false });
	}
	const edt::asset_batch later = database.apply_changes(watcher);
	BOOST_TEST(later.moved.empty());
	BOOST_TEST(later.added.empty());
	BOOST_TEST(later.removed.empty());
	BOOST_TEST(database.find(res::tag::make("clouds.desc")) != nullptr);
}

BOOST_AUTO_TEST_CASE(TheDatabaseFollowsAnAnnouncedUndo)
{
	edt::asset_database& database = open();
	forward_self_changes = true;
	const edt::operation_report report = run(request(edt::asset_operation::move, "sky.desc", "env"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(database.find(res::tag::make("env/sky.desc")) != nullptr);

	const edt::undo_report undone = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST(database.find(res::tag::make("sky.desc")) != nullptr);
	BOOST_TEST(database.find(res::tag::make("env/sky.desc")) == nullptr);
}

BOOST_AUTO_TEST_CASE(TheHistorySerialsOfTheDatabaseGrow)
{
	edt::asset_database& database = open();
	const std::uint64_t first = database.next_history_serial();
	const std::uint64_t second = database.next_history_serial();
	BOOST_TEST(second > first);
	BOOST_TEST(!database.next_undo_serial().has_value());
}

// #1209: folders and several files, one plan, one journal.
BOOST_AUTO_TEST_CASE(ARenameOfAFolder_MovesItsTree_AndEveryReferenceFollows_FromOutsideAndInside)
{
	make_kit();
	open();
	const auto before = tree();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(holds == 1, "the watcher reports the operation as one batch");
	BOOST_TEST(!fs::exists(layer.path() / "kit"), "the old folder is gone");
	for (const std::string_view file : { "a.desc", "a.desc.resource", "sub/b.desc.resource", "sub/rock.mesh.desc", "sub/rock.mesh.desc.resource",
		"sub/rock.mesh.bin", "sub/rock.mesh.bin.resource" }) {
		BOOST_TEST(text_of("set/" + std::string(file)) == before.at("kit/" + std::string(file)), file);
	}
	BOOST_TEST(value_at("set/sub/b.desc", "/a") == "res://set/a.desc", "a moved file that refers to another is rewritten at its new place");
	BOOST_TEST(value_at("set/sub/b.desc", "/m/material/path") == "res://set/a.desc");
	BOOST_TEST(value_at("set/sub/b.desc", "/m/material/guid") == k_kit_a_guid);
	BOOST_TEST(value_at("levels/kit_user.desc", "/a") == "res://set/a.desc");
	BOOST_TEST(value_at("levels/kit_user.desc", "/rock") == "res://set/sub/rock.mesh.desc");
	BOOST_TEST(value_at("levels/kit_user.desc", "/b/material/path") == "res://set/sub/b.desc");
	BOOST_TEST(value_at("levels/kit_user.desc", "/b/material/guid") == k_kit_b_guid);
	BOOST_TEST(rewritten_in(report, "res://set/sub/b.desc") != nullptr);
	const edt::rewritten_file* user = rewritten_in(report, "res://levels/kit_user.desc");
	BOOST_REQUIRE(user);
	BOOST_TEST(user->json_pointers.size() == 3u, "one rewrite of the file for the three assets it refers to");

	// Originals first, then folders made, moves, rewrites, folders removed.
	const auto rank = [](edt::journal_step_kind kind) {
		switch (kind) {
		case edt::journal_step_kind::make_folder: return 0;
		case edt::journal_step_kind::move:
		case edt::journal_step_kind::stash: return 1;
		case edt::journal_step_kind::rewrite: return 2;
		case edt::journal_step_kind::remove_folder: return 3;
		}
		return 4;
	};
	const std::vector<edt::journal_step>& steps = report.journal.steps;
	BOOST_REQUIRE(!steps.empty());
	BOOST_TEST((steps.front().kind == edt::journal_step_kind::make_folder));
	BOOST_TEST(steps.front().from == "set");
	BOOST_TEST((steps.back().kind == edt::journal_step_kind::remove_folder));
	BOOST_TEST(steps.back().from == "kit", "the folder goes after the one inside it");
	BOOST_TEST(std::ranges::is_sorted(steps, {}, [&](const edt::journal_step& step) { return rank(step.kind); }));
	BOOST_TEST(edt::journal_problem(report.journal).empty());

	const std::optional<edt::operation_journal> loaded = edt::journal_from_json(json::parse(json::serialize(edt::journal_to_json(report.journal))));
	BOOST_REQUIRE_MESSAGE(loaded.has_value(), "the folder steps survive being written as JSON");
	const edt::undo_report undone = ops().undo(*loaded);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST((tree() == before), "every folder and every byte back, the folders it made gone");
}

BOOST_AUTO_TEST_CASE(AMoveOfAFolder_PutsItInsideTheTarget_AndItsUndoRemovesWhatItMade)
{
	make_kit();
	open();
	const auto before = tree();
	const edt::operation_report report = run(request(edt::asset_operation::move, "kit", "env"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(has("env/kit/a.desc"));
	BOOST_TEST(has("env/kit/sub/rock.mesh.bin"));
	BOOST_TEST(has("env/.keep"), "what the target held stays");
	BOOST_TEST(!fs::exists(layer.path() / "kit"));
	BOOST_TEST(value_at("env/kit/sub/b.desc", "/a") == "res://env/kit/a.desc");
	BOOST_TEST(value_at("levels/kit_user.desc", "/rock") == "res://env/kit/sub/rock.mesh.desc");

	const edt::undo_report undone = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST((tree() == before));
	BOOST_TEST(!fs::exists(layer.path() / "env" / "kit"));
}

BOOST_AUTO_TEST_CASE(AMoveOfFilesFromDifferentFolders_IsOnePlan_AndAReferrerOfTwoOfThemIsRewrittenOnce)
{
	make_kit();
	open();
	const auto before = tree();
	const edt::asset_request asked[] = { request(edt::asset_operation::move, "kit/a.desc", "env"),
		request(edt::asset_operation::move, "kit/sub/rock.mesh.desc", "env"), request(edt::asset_operation::move, "sky.desc", "env") };
	edt::asset_ops operations = ops();
	const edt::operation_plan planned = operations.plan(asked);
	BOOST_REQUIRE_MESSAGE(!planned.refused.has_value(), planned.refused ? planned.refused->reason : std::string{});
	BOOST_TEST(planned.items.size() == 3u);
	BOOST_TEST(planned.make_folders.empty());
	BOOST_TEST(planned.remove_folders.empty(), "a file asked by itself never takes its folder away");
	const edt::operation_report report = operations.execute(planned);
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(holds == 1);
	for (const std::string_view file : { "env/a.desc", "env/a.desc.resource", "env/rock.mesh.desc", "env/rock.mesh.bin", "env/rock.mesh.bin.resource", "env/sky.desc" }) {
		BOOST_TEST(has(file), file);
	}
	BOOST_TEST(fs::is_directory(layer.path() / "kit" / "sub"));
	const auto user_rewrites = std::ranges::count(report.rewritten, std::string("res://levels/kit_user.desc"), &edt::rewritten_file::file);
	BOOST_TEST(user_rewrites == 1);
	const edt::rewritten_file* user = rewritten_in(report, "res://levels/kit_user.desc");
	BOOST_REQUIRE(user);
	BOOST_TEST(user->json_pointers.size() == 2u);
	const auto journaled = std::ranges::count_if(report.journal.steps, [](const edt::journal_step& step) {
		return step.kind == edt::journal_step_kind::rewrite && step.from == "levels/kit_user.desc";
	});
	BOOST_TEST(journaled == 1);
	BOOST_TEST(value_at("levels/kit_user.desc", "/a") == "res://env/a.desc");
	BOOST_TEST(value_at("levels/kit_user.desc", "/rock") == "res://env/rock.mesh.desc");
	BOOST_TEST(value_at("kit/sub/b.desc", "/a") == "res://env/a.desc");
	BOOST_TEST(value_at("levels/main.desc", k_node + "/sky") == "res://env/sky.desc");

	const edt::undo_report undone = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST((tree() == before));
}

BOOST_AUTO_TEST_CASE(ADeleteOfAFolder_CountsOnlyTheReferrersOutsideIt_AndIsTakenBackWhole)
{
	make_kit();
	open();
	const auto before = tree();
	const edt::operation_report refused = run(request(edt::asset_operation::remove, "kit"));
	BOOST_REQUIRE(refused.refused.has_value());
	BOOST_TEST((refused.refused->code == edt::refusal_code::has_referrers));
	BOOST_TEST(refused.refused->referrers.size() == 3u);
	for (const edt::asset_usage& usage : refused.refused->referrers) {
		BOOST_TEST(usage.file == "res://levels/kit_user.desc", "b.desc goes too: what it refers to does not count");
	}
	BOOST_TEST((tree() == before));

	const edt::operation_report forced = run(request(edt::asset_operation::remove, "kit"), true);
	BOOST_REQUIRE_MESSAGE(forced.ok, forced.error);
	BOOST_TEST(!fs::exists(layer.path() / "kit"));
	BOOST_TEST(fs::is_regular_file(journal.path() / forced.operation_id / "files" / "kit" / "sub" / "rock.mesh.bin"), "kept in its folders");
	BOOST_TEST(text_of("levels/kit_user.desc") == before.at("levels/kit_user.desc"));
	BOOST_TEST(forced.broken_references.size() == 3u);
	BOOST_TEST(edt::journal_problem(forced.journal).empty());

	const edt::undo_report undone = ops().undo(forced.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST((tree() == before));
}

BOOST_AUTO_TEST_CASE(AFolderWithFilesTheProjectDoesNotList_IsNotDeleted_ButMovesWithThemAsTheyAre)
{
	make_kit();
	write("kit/draft.tmp", "scratch");
	write("kit/.git/HEAD", "ref: refs/heads/main");
	open();
	const auto before = tree();
	const edt::operation_report removal = run(request(edt::asset_operation::remove, "kit"), true);
	BOOST_REQUIRE(removal.refused.has_value());
	BOOST_TEST((removal.refused->code == edt::refusal_code::folder_has_foreign_entries));
	const auto listed = [&](std::string_view entry) { return std::ranges::find(removal.refused->entries, entry) != removal.refused->entries.end(); };
	BOOST_TEST(listed("res://kit/draft.tmp"));
	BOOST_TEST(listed("res://kit/.git/HEAD"));
	BOOST_TEST(removal.refused->reason.find("draft.tmp") != std::string::npos, removal.refused->reason);
	BOOST_TEST((tree() == before), "nothing is deleted");

	const edt::operation_report renamed = run(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_REQUIRE_MESSAGE(renamed.ok, renamed.error);
	BOOST_TEST(text_of("set/draft.tmp") == "scratch");
	BOOST_TEST(text_of("set/.git/HEAD") == "ref: refs/heads/main");
	BOOST_TEST(!fs::exists(layer.path() / "kit"));
	const auto carried = std::ranges::find(renamed.journal.steps, std::string("kit/.git/HEAD"), &edt::journal_step::from);
	BOOST_REQUIRE(carried != renamed.journal.steps.end());
	BOOST_TEST((carried->kind == edt::journal_step_kind::move));
	BOOST_TEST(carried->to == "set/.git/HEAD");
	BOOST_TEST(!carried->hash.empty(), "a carried file is moved by its content hash");
	BOOST_TEST(edt::journal_problem(renamed.journal).empty(), "a file carried along is a move the journal accepts");

	const edt::undo_report undone = ops().undo(renamed.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST((tree() == before));
}

BOOST_AUTO_TEST_CASE(ALinkInAFolder_IsNeverFollowedNorMoved_TheOperationIsRefused)
{
	make_kit();
	std::error_code link_error;
	fs::create_directory_symlink(foreign.path(), layer.path() / "kit" / "outside", link_error);
	if (link_error) return;
	open();
	for (const edt::asset_operation operation : { edt::asset_operation::rename, edt::asset_operation::move, edt::asset_operation::remove }) {
		const edt::operation_report report = run(request(operation, "kit", operation == edt::asset_operation::move ? "env" : "set"), true);
		BOOST_REQUIRE_MESSAGE(report.refused.has_value(), edt::asset_operation_name(operation));
		BOOST_TEST((report.refused->code == edt::refusal_code::folder_has_foreign_entries), edt::asset_operation_name(operation));
		BOOST_TEST(report.refused->reason.find("kit/outside") != std::string::npos, report.refused->reason);
	}
	BOOST_TEST(fs::is_symlink(layer.path() / "kit" / "outside"));
	BOOST_TEST(has("kit/a.desc"));
	BOOST_TEST(holds == 0);
}

BOOST_AUTO_TEST_CASE(AnEmptyFolder_IsRenamedAndDeleted_WithFolderStepsOnly)
{
	fs::create_directories(layer.path() / "hollow" / "inner");
	open();
	const auto before = tree();
	const edt::operation_report renamed = run(request(edt::asset_operation::rename, "hollow", "void"));
	BOOST_REQUIRE_MESSAGE(renamed.ok, renamed.error);
	BOOST_TEST(fs::is_directory(layer.path() / "void" / "inner"));
	BOOST_TEST(!fs::exists(layer.path() / "hollow"));
	BOOST_TEST(renamed.journal.steps.size() == 4u);
	BOOST_TEST(std::ranges::all_of(renamed.journal.steps, [](const edt::journal_step& step) {
		return step.kind == edt::journal_step_kind::make_folder || step.kind == edt::journal_step_kind::remove_folder;
	}));
	const edt::undo_report back = ops().undo(renamed.journal);
	BOOST_REQUIRE_MESSAGE(back.ok, back.error);
	BOOST_TEST((tree() == before));

	const edt::operation_report removal = run(request(edt::asset_operation::remove, "hollow"));
	BOOST_REQUIRE_MESSAGE(removal.ok, removal.error);
	BOOST_TEST(!fs::exists(layer.path() / "hollow"));
	const edt::undo_report again = ops().undo(removal.journal);
	BOOST_REQUIRE_MESSAGE(again.ok, again.error);
	BOOST_TEST((tree() == before));
}

BOOST_AUTO_TEST_CASE(AFailureInTheMiddleOfAFolderOperation_TakesEveryStepBack)
{
	make_kit();
	open();
	const auto before = tree();
	for (const std::string_view step : { "make_folder:set/sub", "move:kit/sub/b.desc", "move:kit/sub/rock.mesh.desc", "rewrite:set/sub/b.desc",
		"rewrite:levels/kit_user.desc", "remove_folder:kit" }) {
		fail_at = std::string(step);
		const edt::operation_report report = run(request(edt::asset_operation::rename, "kit", "set"));
		BOOST_TEST(!report.ok, step);
		BOOST_TEST(report.error == "injected failure", step);
		BOOST_TEST(report.rollback_failures.empty(), step);
		BOOST_TEST((tree() == before), std::string("everything is as it was after a failure at ") + std::string(step));
	}
	fail_at = "stash:kit/sub/rock.mesh.desc";
	const edt::operation_report removal = run(request(edt::asset_operation::remove, "kit"), true);
	BOOST_TEST(!removal.ok);
	BOOST_TEST(removal.rollback_failures.empty());
	BOOST_TEST((tree() == before), "a delete of a folder that fails half way is taken back");
}

BOOST_AUTO_TEST_CASE(FolderRequestsThatCannotBeDone_AreRefusedWithTheirOwnCode_AndOverlapsThatAgreeAreOne)
{
	make_kit();
	write("old/new.desc", canonical(R"({ "__type": "material_desc" })"));
	lower.write("old/left.desc", canonical(R"({ "__type": "material_desc" })"));
	lower.write("lowonly/y.desc", canonical(R"({ "__type": "material_desc" })"));
	open(false, edt::platform_case_insensitive, true);
	const auto before = tree();
	const auto code_of = [this](std::span<const edt::asset_request> asked) {
		const edt::operation_plan planned = ops().plan(asked);
		return planned.refused ? std::optional<edt::refusal_code>(planned.refused->code) : std::nullopt;
	};
	const edt::asset_request mixed[] = { request(edt::asset_operation::move, "kit", "env"), request(edt::asset_operation::remove, "sky.desc") };
	BOOST_TEST((code_of(mixed) == edt::refusal_code::mixed_operations));
	const edt::asset_request apart[] = { request(edt::asset_operation::move, "kit", "env"), request(edt::asset_operation::move, "kit/a.desc", "levels") };
	BOOST_TEST((code_of(apart) == edt::refusal_code::overlapping_requests));
	for (const std::string_view into : { "kit", "kit/sub", "res://kit/sub/" }) {
		const edt::asset_request itself[] = { request(edt::asset_operation::move, "kit", into) };
		BOOST_TEST((code_of(itself) == edt::refusal_code::into_itself), into);
	}
	const edt::asset_request chained[] = { request(edt::asset_operation::move, "kit/sub", "env"), request(edt::asset_operation::move, "env", "levels") };
	BOOST_TEST((code_of(chained) == edt::refusal_code::overlapping_requests), "a folder moved into a folder that moves itself");
	const edt::asset_request into_moving[] = { request(edt::asset_operation::move, "kit", "env"), request(edt::asset_operation::move, "sky.desc", "kit/sub") };
	BOOST_TEST((code_of(into_moving) == edt::refusal_code::overlapping_requests), "a target inside another folder being moved");
	const edt::operation_plan lower_target = plan(request(edt::asset_operation::rename, "kit", "lowonly"));
	BOOST_REQUIRE(lower_target.refused.has_value());
	BOOST_TEST((lower_target.refused->code == edt::refusal_code::folder_in_lower_layer), "a destination folder only a lower layer has");
	const edt::operation_plan lower_layer = plan(request(edt::asset_operation::remove, "old"));
	BOOST_REQUIRE(lower_layer.refused.has_value());
	BOOST_TEST((lower_layer.refused->code == edt::refusal_code::folder_in_lower_layer));
	BOOST_TEST((lower_layer.refused->entries == std::vector<std::string>{ "res://old/left.desc" }));
	BOOST_TEST(lower_layer.refused->reason.find("old/left.desc") != std::string::npos);

	const edt::asset_request agreeing[] = { request(edt::asset_operation::remove, "kit"), request(edt::asset_operation::remove, "kit/a.desc"),
		request(edt::asset_operation::remove, "kit/sub/rock.mesh.bin"), request(edt::asset_operation::remove, "kit/a.desc.resource"),
		request(edt::asset_operation::remove, "kit/sub") };
	const edt::operation_plan one = ops().plan(agreeing);
	BOOST_REQUIRE_MESSAGE(!one.refused.has_value(), one.refused ? one.refused->reason : std::string{});
	BOOST_TEST(one.items.size() == 3u, "a file, its folder, its .resource and a desc's binary file are one item");
	BOOST_TEST((one.remove_folders == std::vector<std::string>{ "kit/sub", "kit" }));
	BOOST_TEST((tree() == before));
}

BOOST_AUTO_TEST_CASE(AnOperationOverTheLimits_IsRefused_SayingWhatItCounted)
{
	make_kit();
	open();
	limits.max_files = 3;
	const edt::operation_plan files = plan(request(edt::asset_operation::remove, "kit"));
	BOOST_REQUIRE(files.refused.has_value());
	BOOST_TEST((files.refused->code == edt::refusal_code::too_many_files));
	BOOST_TEST(files.refused->reason.find("at most 3 files") != std::string::npos, files.refused->reason);

	limits = {};
	limits.max_bytes = 10;
	const edt::operation_plan bytes = plan(request(edt::asset_operation::move, "kit", "env"));
	BOOST_REQUIRE(bytes.refused.has_value());
	BOOST_TEST((bytes.refused->code == edt::refusal_code::too_many_files));
	BOOST_TEST(bytes.refused->reason.find("10 file(s) and folder(s)") != std::string::npos, bytes.refused->reason);
	BOOST_TEST(bytes.refused->reason.find("and 10 bytes") != std::string::npos, bytes.refused->reason);

	limits = {};
	limits.max_files = 2;
	const edt::operation_plan pair = plan(request(edt::asset_operation::rename, "meshes/rock.mesh.desc", "stone.mesh.desc"));
	BOOST_REQUIRE(pair.refused.has_value());
	BOOST_TEST((pair.refused->code == edt::refusal_code::too_many_files));
	BOOST_TEST(pair.refused->reason.find("4 file(s)") != std::string::npos, pair.refused->reason);
	BOOST_TEST(has("meshes/rock.mesh.desc"));
}

BOOST_AUTO_TEST_CASE(AFolderRenamedInCaseOnly_OrToAFolderTakenUpToCase_IsRefused_WhereNamesIgnoreCase)
{
	make_kit();
	write("Set/x.desc", canonical(R"({ "__type": "material_desc" })"));
	write("env/KIT/y.desc", canonical(R"({ "__type": "material_desc" })"));
	open(false, true);
	const auto before = tree();
	const edt::operation_plan case_only = plan(request(edt::asset_operation::rename, "kit", "Kit"));
	BOOST_REQUIRE(case_only.refused.has_value());
	BOOST_TEST((case_only.refused->code == edt::refusal_code::case_only_rename));
	const edt::operation_plan taken = plan(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_REQUIRE(taken.refused.has_value());
	BOOST_TEST((taken.refused->code == edt::refusal_code::target_exists));
	const edt::operation_plan inside = plan(request(edt::asset_operation::move, "kit", "env"));
	BOOST_REQUIRE(inside.refused.has_value());
	BOOST_TEST((inside.refused->code == edt::refusal_code::target_exists), "a folder taken up to case below the target");
	BOOST_TEST((tree() == before));
}

BOOST_AUTO_TEST_CASE(AJournalWithAForgedFolderStep_IsRefused_AndNothingIsTouched)
{
	make_kit();
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(edt::journal_problem(report.journal).empty());
	const auto after = tree();
	for (const edt::journal_step_kind kind : { edt::journal_step_kind::make_folder, edt::journal_step_kind::remove_folder }) {
		const auto found = std::ranges::find(report.journal.steps, kind, &edt::journal_step::kind);
		BOOST_REQUIRE(found != report.journal.steps.end());
		const std::size_t index = static_cast<std::size_t>(found - report.journal.steps.begin());
		for (const std::string_view bad : { "/etc", "../outside", "set/../../outside", "C:/x", "a\\b", "", ".git", "set//x", "levels/.." }) {
			edt::operation_journal forged = report.journal;
			forged.steps[index].from = std::string(bad);
			BOOST_TEST(!edt::journal_problem(forged).empty(), bad);
			std::string reason;
			BOOST_TEST(!edt::journal_from_json(edt::journal_to_json(forged), reason).has_value(), bad);
			BOOST_TEST(!reason.empty(), bad);
			BOOST_TEST(!ops().undo(forged).ok, bad);
		}
		edt::operation_journal with_hash = report.journal;
		with_hash.steps[index].hash = "fnv1a64:0000000000000000";
		BOOST_TEST(!edt::journal_problem(with_hash).empty(), "a folder step has no hash");
		edt::operation_journal with_target = report.journal;
		with_target.steps[index].to = "elsewhere";
		BOOST_TEST(!edt::journal_problem(with_target).empty(), "a folder step has no target");
	}
	edt::operation_journal outside = report.journal;
	for (edt::journal_step& step : outside.steps) step.root = foreign.path().generic_string();
	BOOST_TEST(!ops().undo(outside).ok, "a folder step outside the project's own folder");
	BOOST_TEST((tree() == after));
}

BOOST_AUTO_TEST_CASE(ADatabaseBehindTheDisk_RefusesAFolderOperation_InsteadOfCarryingAnAssetAlongAsForeign)
{
	make_kit();
	open();
	write("kit/sub/late.geom.desc", canonical(R"({ "__type": "mesh_desc" })"));
	write("kit/sub/late.geom.bin", "geometry");
	const auto before = tree();
	for (const edt::asset_operation operation : { edt::asset_operation::rename, edt::asset_operation::move, edt::asset_operation::remove }) {
		const edt::operation_plan planned = plan(request(operation, "kit", operation == edt::asset_operation::move ? "env" : "set"));
		BOOST_REQUIRE_MESSAGE(planned.refused.has_value(), edt::asset_operation_name(operation));
		BOOST_TEST((planned.refused->code == edt::refusal_code::database_not_ready), edt::asset_operation_name(operation));
		BOOST_TEST(planned.refused->reason.find("kit/sub/late.geom.desc") != std::string::npos, planned.refused->reason);
	}
	BOOST_TEST((tree() == before));

	// Before its first walk the database knows nothing: no operation is planned at all.
	db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ engine.path(), layer.path() });
	db->rebuild();
	const edt::operation_plan early = plan(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE(early.refused.has_value());
	BOOST_TEST((early.refused->code == edt::refusal_code::database_not_ready));
	test_util::pump(tasks);
	BOOST_TEST(!plan(request(edt::asset_operation::rename, "sky.desc", "clouds.desc")).refused.has_value(), "once it walked, it plans");
}

BOOST_AUTO_TEST_CASE(AMovedFolder_TakesAGeometryDescWithItsBinaryPair_AndParentReferencesInsideIt)
{
	make_kit();
	write("kit/sub/shape.geom.desc", canonical(R"({ "__type": "mesh_desc" })"));
	write("kit/sub/shape.geom.desc.resource", meta_text("e5f60718-5555-4666-8777-88889999aaaa"));
	write("kit/sub/shape.geom.bin", std::string("SNKB\x01\x00\x00\x00 shape", 14));
	write("kit/sub/shape.geom.bin.resource", meta_text("f6071829-6666-4777-8888-9999aaaabbbb"));
	write("kit/sub/child.desc", canonical(R"({ "__type": "material_desc", "__parent": "res://kit/a.desc" })"));
	write("kit/sub/deeper/grand.desc", canonical(R"({ "__type": "material_desc", "__parent": "res://kit/sub/child.desc" })"));
	open();
	const auto before = tree();
	const edt::operation_report report = run(request(edt::asset_operation::move, "kit", "env"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(text_of("env/kit/sub/shape.geom.bin") == before.at("kit/sub/shape.geom.bin"));
	BOOST_TEST(text_of("env/kit/sub/shape.geom.bin.resource") == before.at("kit/sub/shape.geom.bin.resource"));
	BOOST_TEST(text_of("env/kit/sub/shape.geom.desc.resource") == before.at("kit/sub/shape.geom.desc.resource"));
	const auto bin = std::ranges::find(report.journal.steps, std::string("kit/sub/shape.geom.bin"), &edt::journal_step::from);
	const auto desc = std::ranges::find(report.journal.steps, std::string("kit/sub/shape.geom.desc"), &edt::journal_step::from);
	BOOST_REQUIRE(bin != report.journal.steps.end());
	BOOST_TEST((bin < desc), "the binary file goes before its desc");
	BOOST_TEST(value_at("env/kit/sub/child.desc", "/__parent") == "res://env/kit/a.desc");
	BOOST_TEST(value_at("env/kit/sub/deeper/grand.desc", "/__parent") == "res://env/kit/sub/child.desc", "#1460: a parent in the same moved folder");

	const edt::undo_report undone = ops().undo(report.journal);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST((tree() == before));
}

BOOST_AUTO_TEST_CASE(ARollback_LeavesAFolderSomebodyElseMade)
{
	make_kit();
	open();
	on_step = [&](std::string_view step) {
		if (step == "make_folder:set/sub") fs::create_directories(layer.path() / "set" / "sub");
	};
	const edt::operation_report report = run(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_TEST(!report.ok);
	BOOST_TEST(report.rollback_failures.empty());
	BOOST_TEST(fs::is_directory(layer.path() / "set" / "sub"), "the folder that was there before the step is not the operation's to remove");
	BOOST_TEST(has("kit/a.desc"));
	BOOST_TEST(has("kit/sub/b.desc"));
}

BOOST_AUTO_TEST_CASE(AJournalWithAForgedMoveOfFilesTheProjectDoesNotList_IsRefused)
{
	write(std::string(prj::k_manifest_file_name), "{}");
	make_kit();
	write("kit/draft.tmp", "scratch");
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_REQUIRE(edt::journal_problem(report.journal).empty());
	const auto carried = std::ranges::find(report.journal.steps, std::string("kit/draft.tmp"), &edt::journal_step::from);
	BOOST_REQUIRE(carried != report.journal.steps.end());
	const std::size_t index = static_cast<std::size_t>(carried - report.journal.steps.begin());
	const std::pair<std::string_view, std::string_view> forged[] = { { ".hidden/x/draft.tmp", "set/x/draft.tmp" }, { "kit/draft.tmp", ".hidden/draft.tmp" },
		{ "../kit/draft.tmp", "set/draft.tmp" }, { "kit/../draft.tmp", "set/draft.tmp" }, { ".cache/asset_ops/draft.tmp", "set/asset_ops/draft.tmp" },
		{ "code/x/evil.tmp", "set/x/evil.tmp" }, { "kit/draft.tmp", "code/draft.tmp" }, { "draft.tmp", "set/draft.tmp" } };
	const auto after = tree();
	for (const auto& [from, to] : forged) {
		edt::operation_journal bad = report.journal;
		bad.steps[index].from = std::string(from);
		bad.steps[index].to = std::string(to);
		BOOST_TEST(!edt::journal_problem(bad).empty(), std::string(from) + " -> " + std::string(to));
		BOOST_TEST(!ops().undo(bad).ok, from);
	}
	BOOST_TEST((tree() == after));
}

BOOST_AUTO_TEST_CASE(ALongJournal_IsWrittenInStepFiles_AndReadBackWhole)
{
	for (int i = 0; i < 40; ++i) {
		write(std::format("big/f{:02}.desc", i), canonical(R"({ "__type": "material_desc" })"));
		write(std::format("big/f{:02}.desc.resource", i), meta_text(std::format("12345678-0000-4000-8000-{:012x}", i + 1)));
	}
	open();
	const auto before = tree();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "big", "large"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_REQUIRE(report.journal.steps.size() > 64u);
	const fs::path folder = journal.path() / report.operation_id;
	BOOST_TEST(fs::is_regular_file(folder / "steps.1.json"));
	std::ifstream in(folder / "journal.json", std::ios::binary);
	const json::value head = json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
	BOOST_TEST(head.at("steps").as_array().size() == 64u, "journal.json keeps the first steps");
	std::string reason;
	BOOST_TEST(!edt::journal_from_json(head, reason).has_value(), "the first steps alone are not taken for the journal");

	const std::optional<json::value> whole = edt::read_journal(journal.path(), report.operation_id, reason);
	BOOST_REQUIRE_MESSAGE(whole.has_value(), reason);
	const std::optional<edt::operation_journal> loaded = edt::journal_from_json(*whole, reason);
	BOOST_REQUIRE_MESSAGE(loaded.has_value(), reason);
	BOOST_TEST((edt::journal_to_json(*loaded) == edt::journal_to_json(report.journal)));
	const edt::undo_report undone = ops().undo(*loaded);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST((tree() == before));
}

BOOST_AUTO_TEST_CASE(AFolderOperationCutShortByACrash_IsUndoneFromTheHistoryAtTheNextStart)
{
	make_kit();
	open();
	const auto before = tree();
	// Cut short after a few moves; and after the last rewrite was written into the journal but not done.
	for (const std::string_view crash_at : { "move:kit/sub/rock.mesh.desc", "remove_folder:kit/sub" }) {
		start_history();
		on_step = [&](std::string_view step) {
			if (step == crash_at) keep_crash();
		};
		const edt::operation_report report = run(request(edt::asset_operation::rename, "kit", "set"));
		BOOST_REQUIRE_MESSAGE(report.ok, report.error);
		on_step = {};
		history.reset();
		back_to_crash();
		if (crash_at == "remove_folder:kit/sub") write("levels/kit_user.desc", before.at("levels/kit_user.desc"));

		edt::asset_history& restarted = start_history();
		BOOST_REQUIRE_MESSAGE(restarted.size() == 1u, crash_at);
		BOOST_TEST(restarted.next_is_interrupted(), crash_at);
		const edt::asset_fix_result undone = restarted.undo_last(ops());
		BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
		BOOST_TEST((tree() == before), crash_at);
		BOOST_TEST(restarted.size() == 0u);
		history.reset();
	}
}

BOOST_AUTO_TEST_CASE(TheHistoryListsAnOperationWhileItRuns_AndForgetsOneThatWasTakenBack)
{
	make_kit();
	open();
	edt::asset_history& listed = start_history();
	on_step = [&](std::string_view step) {
		if (step == "move:kit/a.desc") BOOST_TEST(listed.next_is_interrupted(), "listed before the first step");
	};
	fail_at = "move:kit/sub/b.desc";
	const edt::operation_report failed = run(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_TEST(!failed.ok);
	BOOST_TEST(listed.size() == 0u, "an operation taken back whole is not in the history");
	fail_at.clear();
	const edt::operation_report done = run(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_REQUIRE_MESSAGE(done.ok, done.error);
	BOOST_TEST(listed.size() == 1u);
	BOOST_TEST(!listed.next_is_interrupted());
	BOOST_TEST(start_history().size() == 1u, "and it is read back as an ordinary entry");
}

BOOST_AUTO_TEST_CASE(AnOrphanBinaryFileTheDatabaseHasNotRead_RefusesTheOperation_ItIsNotCarried)
{
	make_kit();
	open();
	write("kit/sub/nav.mesh.bin", "an orphan binary file: an asset of its own");
	const auto before = tree();
	const edt::operation_plan planned = plan(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_REQUIRE(planned.refused.has_value());
	BOOST_TEST((planned.refused->code == edt::refusal_code::database_not_ready));
	BOOST_TEST(planned.refused->reason.find("kit/sub/nav.mesh.bin") != std::string::npos, planned.refused->reason);
	BOOST_TEST((tree() == before));

	// Once read, it is an asset like any other and goes with its folder.
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	BOOST_TEST(has("set/sub/nav.mesh.bin"));
}

BOOST_AUTO_TEST_CASE(ADatabaseWalkingAgain_RefusesAnOperationUntilItIsDone)
{
	open();
	db->rebuild();
	const edt::operation_plan planned = plan(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE(planned.refused.has_value());
	BOOST_TEST((planned.refused->code == edt::refusal_code::database_not_ready));
	test_util::pump(tasks);
	BOOST_TEST(!plan(request(edt::asset_operation::rename, "sky.desc", "clouds.desc")).refused.has_value());
}

BOOST_AUTO_TEST_CASE(AFolderTheDatabaseDoesNotRead_IsRefusedWithItsReason_NotLeftWaitingForTheDatabase)
{
	write(std::string(prj::k_manifest_file_name), "{}");
	write("code/game.cpp", "int x;");
	write(".hidden/notes.desc", canonical(R"({ "__type": "material_desc" })"));
	std::string deep = "kit";
	for (int i = 0; i < 64; ++i) deep += "/d";
	write(deep + "/far.desc", canonical(R"({ "__type": "material_desc" })"));
	open();
	const auto before = tree();
	for (const std::string_view folder : { "code", ".hidden" }) {
		const edt::operation_plan planned = plan(request(edt::asset_operation::move, folder, "env"));
		BOOST_REQUIRE_MESSAGE(planned.refused.has_value(), folder);
		BOOST_TEST((planned.refused->code == edt::refusal_code::bad_name), folder);
		BOOST_TEST(planned.refused->reason.find("does not read") != std::string::npos, planned.refused->reason);
	}
	const edt::operation_plan too_deep = plan(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_REQUIRE(too_deep.refused.has_value());
	BOOST_TEST((too_deep.refused->code == edt::refusal_code::io_error));
	BOOST_TEST(too_deep.refused->reason.find("folders deep") != std::string::npos, too_deep.refused->reason);
	BOOST_TEST((tree() == before));
}

BOOST_AUTO_TEST_CASE(AnOperationTheHistoryCannotList_IsRefusedBeforeItsFirstStep)
{
	make_kit();
	open();
	const auto before = tree();
	throw_when_listed = true;
	const edt::operation_report thrown = run(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_REQUIRE(thrown.refused.has_value());
	BOOST_TEST((thrown.refused->code == edt::refusal_code::io_error));
	BOOST_TEST(thrown.refused->reason.find("the history threw") != std::string::npos, thrown.refused->reason);
	BOOST_TEST(thrown.journal.steps.empty());
	BOOST_TEST((tree() == before));
	throw_when_listed = false;

	settings.write("blocker", "a file where the history's folder would be");
	history = std::make_unique<edt::asset_history>(journal.path(), settings.path() / "blocker" / "list.json", [this] { return ++history_serial; });
	const edt::operation_report unsaved = run(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_REQUIRE(unsaved.refused.has_value());
	BOOST_TEST((unsaved.refused->code == edt::refusal_code::io_error));
	BOOST_TEST(unsaved.refused->reason.find("not saved") != std::string::npos, unsaved.refused->reason);
	BOOST_TEST(history->size() == 0u, "an entry that could not be saved is not kept");
	BOOST_TEST((tree() == before));
}

BOOST_AUTO_TEST_CASE(AnOperationTakenBackWhole_LetsNoOlderEntryGo)
{
	open();
	edt::asset_history& listed = start_history();
	for (std::size_t i = 0; i < edt::asset_history::max_entries; ++i) {
		edt::operation_journal made;
		made.id = std::format("op{:02}", i);
		made.directory = (journal.path() / made.id).generic_string();
		made.steps.push_back(edt::journal_step{ edt::journal_step_kind::move, "root", "a.desc", "b.desc", "h", {}, {} });
		listed.record(made);
	}
	BOOST_REQUIRE(listed.size() == edt::asset_history::max_entries);
	fail_at = "move:sky.desc.resource";
	const edt::operation_report failed = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_TEST(!failed.ok);
	BOOST_TEST(listed.size() == edt::asset_history::max_entries, "nothing was let go for an operation that left no trace");
	BOOST_TEST((listed.next_operation_id() == std::optional<std::string>("op19")));
	fail_at.clear();
	const edt::operation_report done = run(request(edt::asset_operation::rename, "sky.desc", "clouds.desc"));
	BOOST_REQUIRE_MESSAGE(done.ok, done.error);
	BOOST_TEST(listed.size() == edt::asset_history::max_entries);
	BOOST_TEST((listed.next_operation_id() == std::optional<std::string>(done.operation_id)));
}

BOOST_AUTO_TEST_CASE(ALongOperationCutShort_IsUndoneThroughItsStepFiles_AlsoBetweenAStepFileAndItsHeader)
{
	make_big();
	open();
	const auto before = tree();
	// Step 66 is next: 65 steps are written, the 65th the first of steps.1.json.
	for (const bool header_lost : { false, true }) {
		start_history();
		on_step = [&](std::string_view step) {
			if (step == "move:big/f32.desc") keep_crash();
		};
		const edt::operation_report report = run(request(edt::asset_operation::rename, "big", "large"));
		BOOST_REQUIRE_MESSAGE(report.ok, report.error);
		on_step = {};
		history.reset();
		back_to_crash();
		const fs::path header = journal.path() / report.operation_id / "journal.json";
		BOOST_REQUIRE(fs::is_regular_file(header.parent_path() / "steps.1.json"));
		if (header_lost) {
			// The crash came after steps.1.json was written and before journal.json named it, so its step was not done either.
			std::ifstream in(header, std::ios::binary);
			json::value head = json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
			in.close();
			BOOST_REQUIRE(head.as_object().erase("more_steps") == 1u);
			std::ofstream(header, std::ios::binary | std::ios::trunc) << json::serialize(head);
			fs::rename(layer.path() / "large" / "f31.desc.resource", layer.path() / "big" / "f31.desc.resource");
		}
		edt::asset_history& restarted = start_history();
		BOOST_REQUIRE_MESSAGE(restarted.size() == 1u, header_lost);
		BOOST_TEST(restarted.next_is_interrupted());
		const edt::asset_fix_result undone = restarted.undo_last(ops());
		BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
		BOOST_TEST((tree() == before), header_lost);
		history.reset();
	}
}

BOOST_AUTO_TEST_CASE(AForgedPendingJournal_IsIgnoredAtTheStart)
{
	make_kit();
	open();
	const edt::operation_report report = run(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_REQUIRE_MESSAGE(report.ok, report.error);
	const auto after = tree();
	start_history();
	const auto forged = [&](const std::string& id, json::value value) {
		value.as_object()["id"] = id;
		value.as_object()["directory"] = (journal.path() / id).generic_string();
		journal.write(id + "/journal.json", json::serialize(value));
		forge_pending(id);
	};
	json::value outside = edt::journal_to_json(report.journal);
	for (json::value& step : outside.at("steps").as_array()) step.as_object()["root"] = foreign.path().generic_string();
	forged("outside", outside);
	for (const std::int64_t files : { std::int64_t{ 0 }, std::int64_t{ 1025 }, std::int64_t{ 1 } }) {
		json::value header = edt::journal_to_json(report.journal);
		header.as_object()["more_steps"] = files;
		forged(std::format("more{}", files), header);
	}
	json::value big = edt::journal_to_json(report.journal);
	big.as_object()["padding"] = std::string(std::size_t{ 17 } << 20, 'x');
	forged("huge", big);
	BOOST_REQUIRE(history->size() == 5u);

	edt::asset_history& restarted = start_history();
	BOOST_TEST(restarted.size() == 0u, "a root outside the project, a wrong step count, a missing step file, a journal over 16 MiB");
	BOOST_TEST(!restarted.undo_last(ops()).ok);
	BOOST_TEST((tree() == after));
}

BOOST_AUTO_TEST_CASE(ARollbackThatCouldNotTakeEverythingBack_StaysInTheHistoryAfterARestart)
{
	open();
	start_history();
	fail_at = "stash:lonely.desc.resource";
	on_step = [&](std::string_view step) {
		if (step == "before_undo:lonely.desc") write("lonely.desc", "someone else's file");
	};
	const edt::operation_report report = run(request(edt::asset_operation::remove, "lonely.desc"));
	BOOST_TEST(!report.ok);
	BOOST_REQUIRE(report.rollback_failures.size() == 1u);
	BOOST_TEST(history->next_is_interrupted());
	on_step = {};
	fail_at.clear();
	edt::asset_history& restarted = start_history();
	BOOST_TEST(restarted.size() == 1u);
	BOOST_TEST(restarted.next_is_interrupted(), "what could not be taken back can still be undone after a restart");
}

BOOST_AUTO_TEST_CASE(AnOperationWhoseJournalWouldBeTooBig_IsRefused)
{
	make_kit();
	open();
	limits.max_journal_bytes = 1000;
	const edt::operation_plan planned = plan(request(edt::asset_operation::rename, "kit", "set"));
	BOOST_REQUIRE(planned.refused.has_value());
	BOOST_TEST((planned.refused->code == edt::refusal_code::too_many_files));
	BOOST_TEST(planned.refused->reason.find("journal") != std::string::npos, planned.refused->reason);
}

BOOST_AUTO_TEST_SUITE_END()
