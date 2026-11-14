#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_apply.h>
#include <snk/desc/desc_json_ops.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/project/prj_json_text.h>
#include <snk/project/prj_manifest.h>
#include <snk/platform/plt_beneath.h>
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef SNK_EDITOR_DATA_DIR
#error "SNK_EDITOR_DATA_DIR must point at Editor/data"
#endif

namespace json = boost::json;
namespace fs = std::filesystem;
namespace pmg = prj::migration;

// Applying a migration (#753, EPIC-58 S3) on copies of the fixture of old versions (#750).
namespace {

std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_text(const fs::path& path, const std::string& text)
{
	fs::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary) << text;
}

// The project's own files, without what a migration keeps of itself in .snk/.
std::map<std::string, std::string> content_of(const fs::path& root)
{
	std::map<std::string, std::string> files;
	for (const auto& entry : fs::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file()) continue;
		const std::string relative = fs::relative(entry.path(), root).generic_string();
		if (relative.starts_with(".snk/")) continue;
		files[relative] = read_text(entry.path());
	}
	return files;
}

struct apply_fixture : test_util::scene_env
{
	apply_fixture()
	{
		// The fixture without its newer and broken files: a migration leaves the newer one (partial) and refuses the
		// broken one without --force (S6, pmg_command_tests).
		fs::remove(copy.res() / "newer.desc");
		fs::remove(copy.res() / "broken.desc");
		mount(copy.res());
	}

	void mount(const fs::path& res_dir)
	{
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ res_dir });
	}

	pmg::apply_context context_for(const fs::path& root)
	{
		pmg::apply_context context{ root, *descs, *rs, {}, {}, {}, {}, nullptr, {}, {} };
		context.make_id = [] { return std::string{ "test-migration" }; };
		return context;
	}

	pmg::apply_outcome apply(const fs::path& root, const pmg::apply_context& context)
	{
		return pmg::apply_migration(pmg::plan_migration(root, pmg::scan_project(root, *descs), *descs), context);
	}

	test_util::fixture_copy copy{ "migration_v1" };
};

}

BOOST_FIXTURE_TEST_SUITE(PmgApplyTests, apply_fixture)

BOOST_AUTO_TEST_CASE(TheFixture_MigratesToTheCurrentVersions_WithABackupAndAReport)
{
	const auto before = content_of(copy.root());
	const pmg::apply_outcome outcome = apply(copy.root(), context_for(copy.root()));
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	const pmg::migration_report& report = outcome.report;
	BOOST_TEST_MESSAGE("error: " << report.error);
	BOOST_TEST((report.status == pmg::migration_status::partial)); // module/unknown.desc stays as it is
	BOOST_TEST(report.id == "test-migration");
	BOOST_REQUIRE(report.skipped.size() == 1u);
	BOOST_TEST(report.skipped[0].path == "res/module/unknown.desc");

	const pmg::scan_result again = pmg::scan_project(copy.root(), *descs);
	BOOST_TEST(again.count(pmg::file_class::outdated) == 0u);
	BOOST_TEST(pmg::plan_migration(copy.root(), again, *descs).empty());

	for (const pmg::applied_file& file : report.files) {
		BOOST_TEST_INFO(file.path);
		BOOST_TEST(read_text(copy.root() / ".snk/migrations/test-migration/backup" / file.path) == before.at(file.path));
		BOOST_TEST(!file.hash_after.empty());
		BOOST_TEST(file.hash_before != file.hash_after);
	}
	BOOST_TEST(read_text(copy.root() / "res/module/unknown.desc") == before.at("res/module/unknown.desc"));
	const json::value journal = json::parse(read_text(copy.root() / ".snk/migrations/test-migration/report.json"));
	BOOST_TEST(journal.at("status").as_string() == "partial");
	BOOST_TEST(fs::exists(copy.root() / ".snk/migrations/test-migration/report.md"));
	BOOST_TEST(pmg::unfinished_migrations(copy.root()).empty());
}

// The form desc_resave writes, so `desc_resave --check` on a migrated project finds nothing to change (S6), but for
// the __guid the engine's save makes for a desc without one: a migration makes none (#851).
BOOST_AUTO_TEST_CASE(AMigratedFile_IsWhatTheEngineSaves)
{
	const pmg::apply_outcome outcome = apply(copy.root(), context_for(copy.root()));
	BOOST_REQUIRE(outcome.refused.empty());
	const auto file = std::ranges::find_if(outcome.report.files, [](const auto& f) { return f.path == "res/cameras/cam_v1.desc"; });
	BOOST_REQUIRE(file != outcome.report.files.end());
	BOOST_TEST(file->written_as == "engine");

	core::desc::desc_adapter_t adapter(*rs, *descs);
	const res::tag tag = res::tag::make("cameras/cam_v1.desc");
	auto loaded = rs->require_sync<desc::desc_base>(tag).get_sync();
	BOOST_REQUIRE(loaded);
	BOOST_REQUIRE(read_text(copy.root() / ".snk/migrations/test-migration/backup/res/cameras/cam_v1.desc").find("__guid") == std::string::npos);
	const std::vector<std::byte> bytes = adapter.serialize(tag, loaded);
	json::value saved = json::parse(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
	saved.as_object().erase("__guid");
	const std::vector<std::byte> expected = descs->serialize_to_bytes(saved);
	BOOST_TEST(read_text(copy.res() / "cameras/cam_v1.desc") == std::string(reinterpret_cast<const char*>(expected.data()), expected.size()));
}

BOOST_AUTO_TEST_CASE(ABrokenFile_RefusesTheMigration_NothingIsWritten)
{
	test_util::fixture_copy full{ "migration_v1" };
	// A newer file alone is left as it is (pmg_command_tests): only the invalid one refuses here.
	fs::remove(full.res() / "newer.desc");
	mount(full.res());
	const auto before = content_of(full.root());
	const pmg::apply_outcome outcome = apply(full.root(), context_for(full.root()));
	BOOST_TEST(!outcome.refused.empty());
	BOOST_TEST((content_of(full.root()) == before));
	BOOST_TEST(!fs::exists(full.root() / ".snk"));
}

BOOST_AUTO_TEST_CASE(AFailedWrite_RollsBack_EveryFileIsAsItWas)
{
	const auto before = content_of(copy.root());
	pmg::apply_context context = context_for(copy.root());
	int content_writes = 0;
	context.write = [&content_writes](const fs::path& root, const fs::path& relative, std::string_view text) -> std::optional<std::string> {
		if (!relative.generic_string().starts_with(".snk/") && ++content_writes == 3) return std::string{ "injected failure" };
		return prj::write_project_file(root, relative, text);
	};
	const pmg::apply_outcome outcome = apply(copy.root(), context);
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((outcome.report.status == pmg::migration_status::rolled_back));
	BOOST_TEST(outcome.report.error.find("injected failure") != std::string::npos);
	BOOST_TEST((content_of(copy.root()) == before));
	const json::value journal = json::parse(read_text(copy.root() / ".snk/migrations/test-migration/report.json"));
	BOOST_TEST(journal.at("status").as_string() == "rolled_back");
}

// The watcher is paused before the first change and resumed after the last, also when the migration fails.
BOOST_AUTO_TEST_CASE(TheWatcher_IsPausedAroundTheWrites)
{
	for (const bool fail : { false, true }) {
		test_util::fixture_copy project{ "migration_v1" };
		fs::remove(project.res() / "newer.desc");
		fs::remove(project.res() / "broken.desc");
		mount(project.res());
		std::vector<std::string> events;
		pmg::apply_context context = context_for(project.root());
		context.pause_watch = [&events] { events.push_back("pause"); };
		context.resume_watch = [&events] { events.push_back("resume"); };
		context.write = [&events, fail](const fs::path& root, const fs::path& relative, std::string_view text) -> std::optional<std::string> {
			const std::string path = relative.generic_string();
			if (!path.starts_with(".snk/")) {
				events.push_back("write");
				if (fail) return std::string{ "injected failure" };
			}
			return prj::write_project_file(root, relative, text);
		};
		apply(project.root(), context);
		BOOST_TEST_INFO("fail: " << fail);
		BOOST_REQUIRE(!events.empty());
		const auto first_write = std::ranges::find(events, "write");
		BOOST_REQUIRE(first_write != events.end());
		BOOST_TEST((std::ranges::find(events, "pause") < first_write));
		BOOST_TEST(events.back() == "resume");
		BOOST_TEST(std::ranges::count(events, "pause") == 1);
	}
}

BOOST_AUTO_TEST_CASE(AJournalLeftInProgress_IsFound)
{
	write_text(copy.root() / ".snk/migrations/20261112T000000Z-0.2.0/report.json", R"({"id": "20261112T000000Z-0.2.0", "status": "in_progress"})");
	write_text(copy.root() / ".snk/migrations/done/report.json", R"({"id": "done", "status": "ok"})");
	const std::vector<std::string> unfinished = pmg::unfinished_migrations(copy.root());
	BOOST_REQUIRE(unfinished.size() == 1u);
	BOOST_TEST(unfinished[0] == "20261112T000000Z-0.2.0");
}

BOOST_AUTO_TEST_CASE(ARollback_RestoresTheBackup_AndDeletesTheCreatedFiles)
{
	const auto before = content_of(copy.root());
	const pmg::apply_outcome outcome = apply(copy.root(), context_for(copy.root()));
	BOOST_REQUIRE(outcome.refused.empty());

	// What a project step of S5 records: a file the migration created.
	write_text(copy.res() / "created.resource", "{}");
	const fs::path journal_path = copy.root() / ".snk/migrations/test-migration/report.json";
	json::value journal = json::parse(read_text(journal_path));
	journal.as_object()["created_files"] = json::array{ "res/created.resource" };
	write_text(journal_path, json::serialize(journal));

	BOOST_TEST(!pmg::rollback_migration(copy.root(), "test-migration").has_value());
	BOOST_TEST((content_of(copy.root()) == before));
	BOOST_TEST(!fs::exists(copy.res() / "created.resource"));
	BOOST_TEST(json::parse(read_text(journal_path)).at("status").as_string() == "rolled_back");
}

BOOST_AUTO_TEST_CASE(ARollback_RefusesAFileChangedAfterTheMigration)
{
	const pmg::apply_outcome outcome = apply(copy.root(), context_for(copy.root()));
	BOOST_REQUIRE(outcome.refused.empty());
	write_text(copy.res() / "cameras/cam_v1.desc", R"({"__type":"camera_desc","__version":2,"fov":10})");
	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE(error.has_value());
	BOOST_TEST(error->find("res/cameras/cam_v1.desc") != std::string::npos);
}

// The condition of the review of #753: a file that derives from another keeps its __parent and only its own fields.
BOOST_AUTO_TEST_CASE(AFileWithAParent_KeepsItsParentAndOnlyItsOwnFields)
{
	write_text(copy.res() / "derive/base.prefab.desc",
		R"({"__type":"prefab_desc","__version":4,"children":[{"__id":"00000000000000b1","name":"body"}]})");
	write_text(copy.res() / "derive/variant.prefab.desc",
		R"({"__type":"prefab_desc","__version":3,"__parent":"res://derive/base.prefab.desc","children":[{"__id":"00000000000000c1","name":"extra"}]})");
	write_text(copy.res() / "derive/base_cam.desc", R"({"__type":"camera_desc","__version":2,"fov":60,"near_distance":0.5})");
	write_text(copy.res() / "derive/cam.desc", R"({"__type":"camera_desc","__parent":"res://derive/base_cam.desc","fov":40})");

	const pmg::apply_outcome outcome = apply(copy.root(), context_for(copy.root()));
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_TEST((outcome.report.status == pmg::migration_status::partial));

	// The engine form keeps the root __parent and only the file's own fields (#812), so both are written by the engine.
	std::map<std::string, const pmg::applied_file*> written;
	for (const auto& file : outcome.report.files) written[file.path] = &file;
	for (const std::string path : { "res/derive/variant.prefab.desc", "res/derive/cam.desc" }) {
		BOOST_TEST_INFO(path);
		BOOST_REQUIRE((written[path] != nullptr));
		BOOST_TEST(written[path]->written_as == "engine");
		BOOST_TEST(written[path]->notes.empty());
	}
	const json::value variant = json::parse(read_text(copy.res() / "derive/variant.prefab.desc"));
	BOOST_TEST(variant.at("__parent").as_string() == "res://derive/base.prefab.desc");
	BOOST_TEST(json::serialize(variant).find("\"body\"") == std::string::npos);
	BOOST_TEST(json::serialize(variant).find("\"extra\"") != std::string::npos);

	const json::value camera = json::parse(read_text(copy.res() / "derive/cam.desc"));
	BOOST_TEST(camera.at("__parent").as_string() == "res://derive/base_cam.desc");
	BOOST_TEST(!camera.as_object().contains("near_distance"));
}

// A schema type with a __parent is written by the engine: its __parent, its own fields and what its migration added (#812).
BOOST_AUTO_TEST_CASE(ASchemaTypeWithAParent_IsWrittenByTheEngine_WithItsOwnFields)
{
	descs->register_schema(desc::parse_type_schema(json::parse(R"({"name": "pmg_test_desc", "kind": "component",
		"fields": [{ "name": "hp", "kind": "i32", "default": 0 }, { "name": "armor", "kind": "i32", "default": 0 }]})").as_object()));
	descs->register_migration("pmg_test_desc", 1, "adds armor", [](json::object& obj, desc::migration_context&) { obj["armor"] = 1; });
	write_text(copy.res() / "derive/base_unit.desc", R"({"__type":"pmg_test_desc","__version":2,"hp":5,"armor":0})");
	write_text(copy.res() / "derive/unit.desc", R"({"__type":"pmg_test_desc","__parent":"res://derive/base_unit.desc","hp":7})");

	const pmg::apply_outcome outcome = apply(copy.root(), context_for(copy.root()));
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	const auto file = std::ranges::find_if(outcome.report.files, [](const auto& f) { return f.path == "res/derive/unit.desc"; });
	BOOST_REQUIRE(file != outcome.report.files.end());
	BOOST_TEST(file->written_as == "engine");
	BOOST_TEST(file->notes.empty());
	const json::value unit = json::parse(read_text(copy.res() / "derive/unit.desc"));
	BOOST_TEST(unit.at("__parent").as_string() == "res://derive/base_unit.desc");
	BOOST_TEST(unit.at("hp").as_int64() == 7);
	// Its own fields and what its migration added, nothing of the parent's baked in.
	BOOST_TEST(unit.at("armor").as_int64() == 1);
	for (const auto& member : unit.as_object()) {
		BOOST_TEST_INFO(std::string_view(member.key()));
		BOOST_TEST((member.key().starts_with("__") || member.key() == "hp" || member.key() == "armor"));
	}
}

// #849 writes a desc with a __parent by its own fields (reduce_to_parent); its root __guid is still the file's (#851):
// the same one it had, none when it had none, and no field of the parent comes with either.
BOOST_AUTO_TEST_CASE(AFileWithAParent_KeepsItsGuid_AndGetsNoneItDidNotHave)
{
	descs->register_schema(desc::parse_type_schema(json::parse(R"({"name": "pmg_test_desc", "kind": "component",
		"fields": [{ "name": "hp", "kind": "i32", "default": 0 }, { "name": "armor", "kind": "i32", "default": 0 }]})").as_object()));
	descs->register_migration("pmg_test_desc", 1, "adds armor", [](json::object& obj, desc::migration_context&) { obj["armor"] = 1; });
	constexpr std::string_view base_guid = "00000000-0000-4000-8000-0000000000a1";
	constexpr std::string_view unit_guid = "00000000-0000-4000-8000-0000000000a2";
	write_text(copy.res() / "derive/base_unit.desc", std::format(
		R"({{"__type":"pmg_test_desc","__version":2,"__guid":"{}","hp":5,"armor":0}})", base_guid));
	write_text(copy.res() / "derive/unit.desc", std::format(
		R"({{"__type":"pmg_test_desc","__guid":"{}","__parent":"res://derive/base_unit.desc","hp":7}})", unit_guid));
	write_text(copy.res() / "derive/plain.desc",
		R"({"__type":"pmg_test_desc","__parent":"res://derive/base_unit.desc","hp":9})");

	const pmg::apply_outcome outcome = apply(copy.root(), context_for(copy.root()));
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	for (const std::string path : { "res/derive/unit.desc", "res/derive/plain.desc" }) {
		BOOST_TEST_INFO(path);
		const auto file = std::ranges::find_if(outcome.report.files, [&](const auto& f) { return f.path == path; });
		BOOST_REQUIRE(file != outcome.report.files.end());
		BOOST_TEST(file->written_as == "engine");
	}

	const auto keys_of = [](const json::value& value) {
		std::set<std::string> keys;
		for (const auto& member : value.as_object()) keys.emplace(member.key());
		return keys;
	};
	const json::value unit = json::parse(read_text(copy.res() / "derive/unit.desc"));
	BOOST_TEST(unit.at("__guid").as_string() == unit_guid);
	BOOST_TEST(unit.at("__parent").as_string() == "res://derive/base_unit.desc");
	BOOST_TEST(unit.at("hp").as_int64() == 7);
	BOOST_TEST((keys_of(unit) == std::set<std::string>{ "__type", "__version", "__guid", "__parent", "hp", "armor" }));

	const json::value plain = json::parse(read_text(copy.res() / "derive/plain.desc"));
	BOOST_TEST(plain.at("__parent").as_string() == "res://derive/base_unit.desc");
	BOOST_TEST((keys_of(plain) == std::set<std::string>{ "__type", "__version", "__parent", "hp", "armor" }));
}

// desc_resave and a migration write one form (#851): after the engine's save keep_root_guid leaves the root __guid as
// the file has it. Since #772 the engine's save itself makes none up, so for a desc without one there is nothing to undo.
BOOST_AUTO_TEST_CASE(KeepRootGuid_TheEnginesSave_AddsNoGuid_AndKeepsTheFilesOne)
{
	constexpr std::string_view guid = "00000000-0000-4000-8000-0000000000c1";
	write_text(copy.res() / "guid/none.desc", R"({"__type":"camera_desc","__version":2,"fov":40})");
	write_text(copy.res() / "guid/kept.desc", std::format(R"({{"__type":"camera_desc","__version":2,"__guid":"{}","fov":40}})", guid));
	core::desc::desc_adapter_t adapter(*rs, *descs);
	const auto saved_of = [&](std::string_view name) {
		const res::tag tag = res::tag::make(std::format("guid/{}.desc", name));
		auto loaded = rs->require_sync<desc::desc_base>(tag).get_sync();
		BOOST_REQUIRE(loaded);
		const std::vector<std::byte> bytes = adapter.serialize(tag, loaded);
		return json::parse(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
	};

	json::value none = saved_of("none");
	BOOST_TEST(!none.as_object().contains("__guid"));
	const json::value none_file = json::parse(read_text(copy.res() / "guid/none.desc"));
	BOOST_TEST(!desc::keep_root_guid(none.as_object(), none_file.as_object()));
	BOOST_TEST(!none.as_object().contains("__guid"));

	json::value kept = saved_of("kept");
	const json::value kept_file = json::parse(read_text(copy.res() / "guid/kept.desc"));
	BOOST_TEST(!desc::keep_root_guid(kept.as_object(), kept_file.as_object()));
	BOOST_TEST(kept.as_object().at("__guid").as_string() == guid);
}

// A value the migration gives the child that equals the parent's is the parent's: the engine form leaves it out (#812).
BOOST_AUTO_TEST_CASE(AMigratedValueEqualToTheParents_IsNotWrittenIntoTheChild)
{
	descs->register_schema(desc::parse_type_schema(json::parse(R"({"name": "pmg_test_desc", "kind": "component",
		"fields": [{ "name": "hp", "kind": "i32", "default": 0 }, { "name": "armor", "kind": "i32", "default": 0 }]})").as_object()));
	descs->register_migration("pmg_test_desc", 1, "adds armor", [](json::object& obj, desc::migration_context&) { obj["armor"] = 1; });
	write_text(copy.res() / "derive/base_unit.desc", R"({"__type":"pmg_test_desc","__version":2,"hp":5,"armor":1})");
	write_text(copy.res() / "derive/unit.desc", R"({"__type":"pmg_test_desc","__parent":"res://derive/base_unit.desc","hp":7})");

	const pmg::apply_outcome outcome = apply(copy.root(), context_for(copy.root()));
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	const auto file = std::ranges::find_if(outcome.report.files, [](const auto& f) { return f.path == "res/derive/unit.desc"; });
	BOOST_REQUIRE(file != outcome.report.files.end());
	BOOST_TEST(file->written_as == "engine");
	const json::value unit = json::parse(read_text(copy.res() / "derive/unit.desc"));
	BOOST_TEST(unit.at("__parent").as_string() == "res://derive/base_unit.desc");
	BOOST_TEST(unit.at("hp").as_int64() == 7);
	BOOST_TEST(!unit.as_object().contains("armor"));
}

// A file the rollback cannot put back leaves the migration rollback_failed, which unfinished_migrations reports.
BOOST_AUTO_TEST_CASE(ARestoreThatFails_IsRollbackFailed_AndUnfinished)
{
	pmg::apply_context context = context_for(copy.root());
	std::map<std::string, int> writes;
	context.write = [&writes](const fs::path& root, const fs::path& relative, std::string_view text) -> std::optional<std::string> {
		const std::string path = relative.generic_string();
		if (!path.starts_with(".snk/")) {
			const int count = ++writes[path];
			if (writes.size() == 3u && count == 1) return std::string{ "injected failure" };
			if (count == 2) return std::string{ "injected restore failure" };
		}
		return prj::write_project_file(root, relative, text);
	};
	const pmg::apply_outcome outcome = apply(copy.root(), context);
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((outcome.report.status == pmg::migration_status::rollback_failed));
	BOOST_TEST(outcome.report.error.find("injected restore failure") != std::string::npos);
	const json::value journal = json::parse(read_text(copy.root() / ".snk/migrations/test-migration/report.json"));
	BOOST_TEST(journal.at("status").as_string() == "rollback_failed");
	const std::vector<std::string> unfinished = pmg::unfinished_migrations(copy.root());
	BOOST_TEST((unfinished == std::vector<std::string>{ "test-migration" }));
}

// What check() reads from the disk differs from what was written: the migration rolls back.
BOOST_AUTO_TEST_CASE(ACheckThatFails_RollsBack)
{
	const auto before = content_of(copy.root());
	pmg::apply_context context = context_for(copy.root());
	std::map<std::string, int> writes;
	context.write = [&writes](const fs::path& root, const fs::path& relative, std::string_view text) -> std::optional<std::string> {
		const std::string path = relative.generic_string();
		if (!path.starts_with(".snk/") && ++writes[path] == 1 && writes.size() == 2u) {
			return prj::write_project_file(root, relative, std::string(text) + "\n ");
		}
		return prj::write_project_file(root, relative, text);
	};
	const pmg::apply_outcome outcome = apply(copy.root(), context);
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((outcome.report.status == pmg::migration_status::rolled_back));
	BOOST_TEST(outcome.report.error.find("is not what was written") != std::string::npos);
	BOOST_TEST((content_of(copy.root()) == before));
}

BOOST_AUTO_TEST_CASE(AJournalWriteThatFailsMidway_RollsBack)
{
	const auto before = content_of(copy.root());
	pmg::apply_context context = context_for(copy.root());
	int journal_writes = 0;
	context.write = [&journal_writes](const fs::path& root, const fs::path& relative, std::string_view text) -> std::optional<std::string> {
		if (relative.generic_string().ends_with("/report.json") && ++journal_writes == 3) return std::string{ "injected journal failure" };
		return prj::write_project_file(root, relative, text);
	};
	const pmg::apply_outcome outcome = apply(copy.root(), context);
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((outcome.report.status == pmg::migration_status::rolled_back));
	BOOST_TEST(outcome.report.error.find("injected journal failure") != std::string::npos);
	BOOST_TEST((content_of(copy.root()) == before));
}

// Two migrations that get the same id never share its folder.
BOOST_AUTO_TEST_CASE(ATakenId_GetsASuffix)
{
	write_text(copy.root() / ".snk/migrations/test-migration/report.json", R"({"id": "test-migration", "status": "ok"})");
	const pmg::apply_outcome outcome = apply(copy.root(), context_for(copy.root()));
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST(outcome.report.id == "test-migration-2");
	BOOST_TEST(json::parse(read_text(copy.root() / ".snk/migrations/test-migration/report.json")).at("status").as_string() == "ok");
	BOOST_TEST(fs::exists(copy.root() / ".snk/migrations/test-migration-2/backup"));
}

// A process that stopped in the middle leaves the journal in_progress; its rollback puts the project back.
BOOST_AUTO_TEST_CASE(ARollback_OfAJournalLeftInProgress_RestoresTheProject)
{
	const auto before = content_of(copy.root());
	BOOST_REQUIRE(apply(copy.root(), context_for(copy.root())).refused.empty());
	const fs::path journal_path = copy.root() / ".snk/migrations/test-migration/report.json";
	json::value journal = json::parse(read_text(journal_path));
	journal.as_object()["status"] = "in_progress";
	write_text(journal_path, json::serialize(journal));
	BOOST_TEST((pmg::unfinished_migrations(copy.root()) == std::vector<std::string>{ "test-migration" }));

	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	BOOST_TEST((content_of(copy.root()) == before));
	BOOST_TEST(pmg::unfinished_migrations(copy.root()).empty());
}

// The journal never recorded a write of the file, so a file that is not the original was changed by someone else.
BOOST_AUTO_TEST_CASE(ARollback_RefusesAFileNeverWrittenThatIsNotTheOriginal)
{
	BOOST_REQUIRE(apply(copy.root(), context_for(copy.root())).refused.empty());
	const auto after = content_of(copy.root());
	const fs::path journal_path = copy.root() / ".snk/migrations/test-migration/report.json";
	json::value journal = json::parse(read_text(journal_path));
	for (json::value& file : journal.as_object()["files"].as_array()) {
		if (file.as_object()["path"].as_string() == "res/cameras/cam_v1.desc") file.as_object()["hash_after"] = "";
	}
	write_text(journal_path, json::serialize(journal));

	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE(error.has_value());
	BOOST_TEST(error->find("res/cameras/cam_v1.desc") != std::string::npos);
	BOOST_TEST((content_of(copy.root()) == after));
}

// A rollback that stopped half way leaves files already back to the original; running it again finishes it.
BOOST_AUTO_TEST_CASE(ARollbackThatStopped_CanRunAgain)
{
	const auto before = content_of(copy.root());
	BOOST_REQUIRE(apply(copy.root(), context_for(copy.root())).refused.empty());
	int writes = 0;
	pmg::rollback_context failing;
	failing.write = [&writes](const fs::path& root, const fs::path& relative, std::string_view text) -> std::optional<std::string> {
		if (++writes == 2) return std::string{ "injected failure" };
		return prj::write_project_file(root, relative, text);
	};
	BOOST_REQUIRE(pmg::rollback_migration(copy.root(), "test-migration", failing).has_value());
	BOOST_TEST((content_of(copy.root()) != before));
	BOOST_TEST((pmg::unfinished_migrations(copy.root()) == std::vector<std::string>{ "test-migration" }));
	BOOST_TEST(json::parse(read_text(copy.root() / ".snk/migrations/test-migration/report.json")).at("status").as_string() == "rollback_failed");

	std::vector<std::string> events;
	pmg::rollback_context context;
	context.pause_watch = [&events] { events.push_back("pause"); };
	context.resume_watch = [&events] { events.push_back("resume"); };
	const auto error = pmg::rollback_migration(copy.root(), "test-migration", context);
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	BOOST_TEST((content_of(copy.root()) == before));
	BOOST_TEST((events == std::vector<std::string>{ "pause", "resume" }));
	BOOST_TEST(pmg::unfinished_migrations(copy.root()).empty());
}

// A journal is a file anyone can edit: a created file outside the content roots is refused and stays.
BOOST_AUTO_TEST_CASE(ARollback_RefusesACreatedFileOutsideTheContentRoots)
{
	BOOST_REQUIRE(apply(copy.root(), context_for(copy.root())).refused.empty());
	const auto after = content_of(copy.root());
	const fs::path outside = copy.root().parent_path() / (copy.root().filename().string() + "-outside.txt");
	write_text(outside, "keep");
	write_text(copy.root() / "keep.txt", "keep");
	const fs::path journal_path = copy.root() / ".snk/migrations/test-migration/report.json";
	for (const std::string& path : { "../" + outside.filename().string(), outside.generic_string(), std::string{ "keep.txt" },
			 std::string{ "res/../keep.txt" } }) {
		BOOST_TEST_INFO(path);
		json::value journal = json::parse(read_text(journal_path));
		journal.as_object()["created_files"] = json::array{ json::string(path) };
		write_text(journal_path, json::serialize(journal));
		const auto error = pmg::rollback_migration(copy.root(), "test-migration");
		BOOST_TEST(error.has_value());
		BOOST_TEST(fs::exists(outside));
		BOOST_TEST(fs::exists(copy.root() / "keep.txt"));
	}
	fs::remove(outside);
	fs::remove(copy.root() / "keep.txt");
	BOOST_TEST((content_of(copy.root()) == after));
}

namespace {

// Every file below the root, .snk included: a refused rollback writes nothing anywhere, not even its journal.
std::map<std::string, std::string> everything_of(const fs::path& root)
{
	std::map<std::string, std::string> files;
	for (const auto& entry : fs::recursive_directory_iterator(root)) {
		if (entry.is_regular_file()) files[fs::relative(entry.path(), root).generic_string()] = read_text(entry.path());
	}
	return files;
}

// The journal of the fixture's migration, edited by `edit`, then a rollback: it must be refused with nothing written.
template <class edit_fn>
void expect_refused(const fs::path& root, edit_fn&& edit, std::string_view why)
{
	const fs::path journal_path = root / ".snk/migrations/test-migration/report.json";
	const std::string original = read_text(journal_path);
	json::value journal = json::parse(original);
	edit(journal.as_object());
	write_text(journal_path, json::serialize(journal));
	const auto before = everything_of(root);
	const auto error = pmg::rollback_migration(root, "test-migration");
	BOOST_REQUIRE(error.has_value());
	BOOST_TEST(error->find("nothing was written") != std::string::npos, *error);
	BOOST_TEST(error->find(why) != std::string::npos, *error);
	BOOST_TEST((everything_of(root) == before));
	BOOST_TEST(pmg::check_journal(root, "test-migration").has_value());
	write_text(journal_path, original);
}

}

// #910: the backup is exactly the folder apply_migration writes, whether or not anything is restored.
BOOST_AUTO_TEST_CASE(ARollback_RefusesABackupAnywhereElse)
{
	BOOST_REQUIRE(apply(copy.root(), context_for(copy.root())).refused.empty());
	write_text(copy.root() / ".snk/migrations/other/backup/res/cameras/cam_v1.desc", "{}");
	for (const std::string& backup : std::vector<std::string>{ ".git", "../x", ".snk/migrations/other/backup", "",
			 ".snk/migrations/test-migration/backup/" }) {
		BOOST_TEST_CONTEXT(backup) {
			expect_refused(copy.root(), [&backup](json::object& journal) { journal["backup"] = backup; }, "the backup");
		}
	}
	// Nothing to restore: a wrong backup is refused all the same.
	expect_refused(copy.root(), [](json::object& journal) {
		journal["backup"] = ".git";
		journal["files"] = json::array{};
		journal["created_files"] = json::array{};
	}, "the backup '.git'");
}

// #910: a file of files[] is project.snkproj or lies inside a content root, never below a hidden, build, code or private
// folder; a hash_after "removed" outside them would otherwise create the file from a backup the journal chose.
BOOST_AUTO_TEST_CASE(ARollback_RefusesAFileOutsideTheContentRoots_AndCreatesNothing)
{
	BOOST_REQUIRE(apply(copy.root(), context_for(copy.root())).refused.empty());
	const std::string payload = "#!/bin/sh\necho owned\n";
	const fs::path absolute = copy.root().parent_path() / (copy.root().filename().string() + "-absolute.txt");
	for (const std::string& path : std::vector<std::string>{ ".git/hooks/pre-commit", ".snk/build.json", "code/a.cpp", "CODE/a.cpp",
			 "build/x", "Build/x", ".cache/x", ".github/x", "other/x.desc", absolute.generic_string(), "..", "res/../x" }) {
		BOOST_TEST_CONTEXT(path) {
			if (path.find("..") == std::string::npos && !fs::path(path).is_absolute()) {
				write_text(copy.root() / ".snk/migrations/test-migration/backup" / path, payload);
			}
			expect_refused(copy.root(), [&path, &payload](json::object& journal) {
				journal["files"].as_array().push_back(json::object{ { "path", path }, { "hash_before", pmg::journal_hash(payload) },
					{ "hash_after", "removed" }, { "written_as", "json" } });
			}, "'" + pmg::journal_path_text(path) + "'");
			BOOST_TEST(!fs::exists(copy.root() / path));
			BOOST_TEST(!fs::exists(absolute));
		}
	}
}

// #910: a created file is checked as files[] are (project.snkproj is no created file): it is refused and stays.
BOOST_AUTO_TEST_CASE(ARollback_RefusesACreatedFileInAReservedFolder)
{
	BOOST_REQUIRE(apply(copy.root(), context_for(copy.root())).refused.empty());
	for (const std::string& path : std::vector<std::string>{ ".git/config", "code/CMakeLists.txt", "build/x", ".snk/agent_policy.json",
			 "project.snkproj" }) {
		BOOST_TEST_CONTEXT(path) {
			if (path != "project.snkproj") write_text(copy.root() / path, "keep");
			expect_refused(copy.root(), [&path](json::object& journal) { journal["created_files"] = json::array{ json::string(path) }; },
				"created file");
			BOOST_TEST(fs::exists(copy.root() / path));
		}
	}
}

// #910: a rollback is a recovery tool: a project that would not open (a writable folder that is a file, a broken
// project.snkproj) is rolled back all the same, its content roots taken from the backup's project.snkproj.
BOOST_AUTO_TEST_CASE(ARollback_OfABrokenProject_PutsItBack)
{
	const auto before = content_of(copy.root());
	BOOST_REQUIRE(apply(copy.root(), context_for(copy.root())).refused.empty());
	write_text(copy.root() / ".cache", "not a folder");
	write_text(copy.root() / "project.snkproj", "{");
	BOOST_TEST(!pmg::check_journal(copy.root(), "test-migration").has_value());

	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	fs::remove(copy.root() / ".cache");
	BOOST_TEST((content_of(copy.root()) == before));
}

// #910: project.snkproj deleted after the migration: the journal lists it, so it comes back from the backup.
BOOST_AUTO_TEST_CASE(ARollback_PutsBackAMissingManifest)
{
	const auto before = content_of(copy.root());
	BOOST_REQUIRE(apply(copy.root(), context_for(copy.root())).refused.empty());
	fs::remove(copy.root() / "project.snkproj");
	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	BOOST_TEST((content_of(copy.root()) == before));
}

// #910: neither the backup's nor the current project.snkproj reads: the content roots are unknown, nothing is written and
// the refusal names the backup to restore by hand.
BOOST_AUTO_TEST_CASE(ARollback_WithoutAManifestThatReads_IsRefused_NamingTheBackup)
{
	BOOST_REQUIRE(apply(copy.root(), context_for(copy.root())).refused.empty());
	write_text(copy.root() / "project.snkproj", "{");
	write_text(copy.root() / ".snk/migrations/test-migration/backup/project.snkproj", "not json");
	expect_refused(copy.root(), [](json::object&) {}, ".snk/migrations/test-migration/backup");
}

// #910: the content roots of the manifest pass the lexical checks of an opened project: inside it, not the root itself,
// not in one of its own folders.
BOOST_AUTO_TEST_CASE(ARollback_RefusesContentRootsInTheProjectsOwnFolders)
{
	BOOST_REQUIRE(apply(copy.root(), context_for(copy.root())).refused.empty());
	const fs::path saved = copy.root() / ".snk/migrations/test-migration/backup/project.snkproj";
	const std::string original = read_text(saved);
	for (const std::string& root : std::vector<std::string>{ ".", ".git", "code", "Build/x", ".cache/memory" }) {
		BOOST_TEST_CONTEXT(root) {
			json::value manifest = json::parse(original);
			manifest.as_object()["content_roots"] = json::array{ json::string(root) };
			write_text(saved, json::serialize(manifest));
			if (prj::parse_manifest(json::serialize(manifest)).data) expect_refused(copy.root(), [](json::object&) {}, "content root");
		}
	}
	write_text(saved, original);
}

// #910: the migration's folder is reached without following a link: a link there would lead the backup reads elsewhere.
BOOST_AUTO_TEST_CASE(ARollback_RefusesAMigrationFolderThatIsALink)
{
	BOOST_REQUIRE(apply(copy.root(), context_for(copy.root())).refused.empty());
	const fs::path folder = copy.root() / ".snk/migrations/test-migration";
	const fs::path moved = copy.root().parent_path() / (copy.root().filename().string() + "-moved-migration");
	fs::rename(folder, moved);
	std::error_code ec;
	fs::create_directory_symlink(moved, folder, ec);
	if (ec) {
		BOOST_TEST_MESSAGE("no symlink here (" << ec.message() << "): skipped");
		fs::rename(moved, folder);
		return;
	}
	const auto before = everything_of(copy.root());
	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE(error.has_value());
	BOOST_TEST(error->find("not a folder of the project") != std::string::npos, *error);
	BOOST_TEST(pmg::check_journal(copy.root(), "test-migration").has_value());
	BOOST_TEST((everything_of(copy.root()) == before));
	fs::remove(folder);
	fs::remove_all(moved);
}

// #910: what a message or a question shows of a journal's path: no character that could forge or hide a part of a line,
// an escape in its place, and a cut that splits neither a code point nor an escape.
BOOST_AUTO_TEST_CASE(AJournalPath_IsShownWithItsHiddenCharactersEscaped)
{
	BOOST_TEST(pmg::journal_path_text("res/a.desc") == "res/a.desc");
	BOOST_TEST(pmg::journal_path_text("a\nb\xE2\x80\xAE" "c\xE2\x80\x8B" "d") == "a\\u{000A}b\\u{202E}c\\u{200B}d");
	BOOST_TEST(pmg::journal_path_text("\xC2\x85\xC2\xAD\xEF\xBB\xBF\xE2\x80\xA8\xE2\x81\xA6\xD8\x9C\x7F") ==
		"\\u{0085}\\u{00AD}\\u{FEFF}\\u{2028}\\u{2066}\\u{061C}\\u{007F}");
	BOOST_TEST(pmg::journal_path_text("a\xFF" "b\xC3") == "a\\xFFb\\xC3");
	BOOST_TEST(pmg::journal_path_text("a\\b") == "a\\\\b");
	BOOST_TEST(pmg::journal_path_text("caf\xC3\xA9") == "caf\xC3\xA9");
	BOOST_TEST(pmg::journal_path_text(std::string(8191, 'a') + "\xC3\xA9") == std::string(8191, 'a') + "...");
	BOOST_TEST(pmg::journal_path_text(std::string(8187, 'a') + "\n") == std::string(8187, 'a') + "...");
	BOOST_TEST(pmg::journal_path_text(std::string(8192, 'a')) == std::string(8192, 'a'));
	// The longest path a journal may name, every byte escaped, is still shown whole.
	std::string escaped_whole;
	for (int i = 0; i < 1024; ++i) escaped_whole += "\\u{0001}";
	BOOST_TEST(pmg::journal_path_text(std::string(1024, '\x01')) == escaped_whole);
}

// #910 review: fill characters, variation selectors, tags and spaces that look like U+0020 are escaped too.
BOOST_AUTO_TEST_CASE(AJournalPath_EscapesFillersSelectorsTagsAndOtherSpaces)
{
	const std::vector<std::pair<std::string, std::string>> cases{
		{ "\xE2\x81\xAA", "206A" }, { "\xE2\x81\xAF", "206F" }, { "\xE1\x85\x9F", "115F" }, { "\xE1\x85\xA0", "1160" },
		{ "\xE3\x85\xA4", "3164" }, { "\xEF\xBE\xA0", "FFA0" }, { "\xEF\xB8\x80", "FE00" }, { "\xEF\xB8\x8F", "FE0F" },
		{ "\xF3\xA0\x80\x81", "E0001" }, { "\xF3\xA0\x80\xA0", "E0020" }, { "\xF3\xA0\x81\xBF", "E007F" }, { "\xC2\xA0", "00A0" },
		{ "\xE2\x80\x80", "2000" }, { "\xE2\x80\x8A", "200A" }, { "\xE2\x80\xAF", "202F" }, { "\xE2\x81\x9F", "205F" },
		{ "\xE3\x80\x80", "3000" },
	};
	for (const auto& [text, code] : cases) {
		BOOST_TEST_CONTEXT(code) {
			BOOST_TEST(pmg::journal_path_text("a" + text + "b") == "a\\u{" + code + "}b");
		}
	}
	// A letter of another script stays as it is.
	BOOST_TEST(pmg::journal_path_text("\xD0\xB6") == "\xD0\xB6");
}

// #910 review: a journal's path longer than 1024 bytes is refused, so every path a question lists is shown whole.
BOOST_AUTO_TEST_CASE(ARollback_RefusesAPathTooLongToShow)
{
	BOOST_REQUIRE(apply(copy.root(), context_for(copy.root())).refused.empty());
	const std::string path = "res/" + std::string(1100, 'x') + ".desc";
	expect_refused(copy.root(), [&path](json::object& journal) {
		journal["files"].as_array().push_back(json::object{ { "path", path }, { "hash_before", "0" }, { "hash_after", "removed" } });
	}, "longer than 1024 bytes");
	expect_refused(copy.root(), [&path](json::object& journal) { journal["created_files"] = json::array{ json::string(path) }; },
		"longer than 1024 bytes");
}

// #910 review: a name with '~' and a digit is an ordinary name; aliases are stopped by the Windows primitives (lies_at),
// so a project that migrates such a file can roll it back too.
BOOST_AUTO_TEST_CASE(AFileNamedLikeAShortName_MigratesAndRollsBack)
{
	write_text(copy.res() / "IMG~1/cam~1.desc", read_text(copy.res() / "cameras/cam_v1.desc"));
	const auto before = content_of(copy.root());
	const pmg::apply_outcome outcome = apply(copy.root(), context_for(copy.root()));
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_REQUIRE(std::ranges::any_of(outcome.report.files, [](const auto& f) { return f.path == "res/IMG~1/cam~1.desc"; }));
	BOOST_TEST(read_text(copy.res() / "IMG~1/cam~1.desc") != before.at("res/IMG~1/cam~1.desc"));

	write_text(copy.res() / "IMG~1.PNG", "png");
	const fs::path journal_path = copy.root() / ".snk/migrations/test-migration/report.json";
	json::value journal = json::parse(read_text(journal_path));
	journal.as_object()["created_files"] = json::array{ "res/IMG~1.PNG" };
	write_text(journal_path, json::serialize(journal));

	BOOST_TEST(!pmg::check_journal(copy.root(), "test-migration").has_value());
	BOOST_TEST(!pmg::rollback_migration(copy.root(), "test-migration").has_value());
	BOOST_TEST((content_of(copy.root()) == before));
	BOOST_TEST(!fs::exists(copy.res() / "IMG~1.PNG"));
}

// #910 review: the name rule a read falls back on when Windows gives no final path.
BOOST_AUTO_TEST_CASE(AShortNameLookalike_HasATildeAndADigit)
{
	BOOST_TEST(platform::beneath::may_be_short_name("SNK~1"));
	BOOST_TEST(platform::beneath::may_be_short_name("IMG~12.PNG"));
	BOOST_TEST(!platform::beneath::may_be_short_name("a~b.desc"));
	BOOST_TEST(!platform::beneath::may_be_short_name("name~"));
	BOOST_TEST(!platform::beneath::may_be_short_name("plain.desc"));
}

#ifdef _WIN32
namespace {
	fs::path short_name_given(const fs::path& path)
	{
		std::wstring buffer(MAX_PATH + 1, L'\0');
		const DWORD length = ::GetShortPathNameW(path.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0 || length >= buffer.size()) return {};
		const fs::path alias = fs::path(buffer.substr(0, length)).filename();
		return alias == path.filename() ? fs::path{} : alias;
	}

	// The short name of `path`: the one Windows made, else `wanted` set here. Most volumes make no 8.3 names; setting one
	// needs SeRestorePrivilege, which an administrator (a CI runner) has. Empty when neither works.
	void set_restore_privilege(bool enabled)
	{
		HANDLE token = nullptr;
		if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) return;
		TOKEN_PRIVILEGES privileges{};
		privileges.PrivilegeCount = 1;
		if (::LookupPrivilegeValueW(nullptr, L"SeRestorePrivilege", &privileges.Privileges[0].Luid)) {
			privileges.Privileges[0].Attributes = enabled ? SE_PRIVILEGE_ENABLED : 0;
			::AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
		}
		::CloseHandle(token);
	}

	fs::path short_name_of(const fs::path& path, const wchar_t* wanted)
	{
		if (fs::path alias = short_name_given(path); !alias.empty()) return alias;
		set_restore_privilege(true);
		const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		BOOL set = FALSE;
		if (file != INVALID_HANDLE_VALUE) {
			set = ::SetFileShortNameW(file, wanted);
			::CloseHandle(file);
		}
		// The rest of unit_tests runs without the privilege, as a person's editor does.
		set_restore_privilege(false);
		return set ? short_name_given(path) : fs::path{};
	}
}

// #910 review: on Windows the no-follow primitives refuse an alias themselves, in a folder of the chain and at the
// leaf: a delete, a read, a write (no folder is made inside .snk) and a move through a short name do not reach what it
// names, and a write does not replace a file through its short name.
BOOST_AUTO_TEST_CASE(TheBeneathPrimitives_RefuseAShortNameAlias)
{
	write_text(copy.root() / ".snk/x", "keep");
	write_text(copy.root() / "res/a_long_file_name.txt", "kept");
	const fs::path folder_alias = short_name_of(fs::canonical(copy.root() / ".snk"), L"SNK~1");
	const fs::path leaf_alias = short_name_of(fs::canonical(copy.root() / "res/a_long_file_name.txt"), L"ALONGF~1.TXT");
	if (folder_alias.empty() || leaf_alias.empty()) {
		BOOST_TEST_MESSAGE("TheBeneathPrimitives_RefuseAShortNameAlias: no short names on this volume and none can be set: skipped");
		return;
	}
	BOOST_TEST_MESSAGE(std::format("TheBeneathPrimitives_RefuseAShortNameAlias: runs with {} and {}", folder_alias.string(), leaf_alias.string()));

	const prj::project_file_text read = prj::read_project_file(copy.root(), folder_alias / "x");
	BOOST_TEST((!read.exists || !read.error.empty()));
	platform::beneath::pinned_root pinned{ fs::canonical(copy.root()) };
	BOOST_TEST(!platform::beneath::remove_beneath(pinned, folder_alias, "x", true).empty());
	BOOST_TEST(read_text(copy.root() / ".snk/x") == "keep");

	const std::string text = "lock";
	const auto bytes = std::as_bytes(std::span(text));
	platform::beneath::write_request into_folder{};
	into_folder.folder = folder_alias / "index.lock";
	into_folder.leaf = "x";
	into_folder.tmp_leaf = "x.tmp";
	into_folder.data = bytes;
	into_folder.tmp_path = copy.root() / folder_alias / "index.lock/x.tmp";
	BOOST_TEST(!platform::beneath::write_beneath(pinned, into_folder).empty());
	BOOST_TEST(!fs::exists(copy.root() / ".snk/index.lock"));

	platform::beneath::rename_request moved{};
	moved.from_folder = "res";
	moved.from_leaf = "a_long_file_name.txt";
	moved.to_folder = folder_alias;
	moved.to_leaf = "moved.txt";
	BOOST_TEST(!platform::beneath::rename_beneath(pinned, moved).empty());
	BOOST_TEST(!fs::exists(copy.root() / ".snk/moved.txt"));

	platform::beneath::write_request over_leaf{};
	over_leaf.folder = "res";
	over_leaf.leaf = leaf_alias;
	over_leaf.tmp_leaf = "replace.tmp";
	over_leaf.data = bytes;
	over_leaf.tmp_path = copy.root() / "res/replace.tmp";
	BOOST_TEST(!platform::beneath::write_beneath(pinned, over_leaf).empty());
	BOOST_TEST(read_text(copy.root() / "res/a_long_file_name.txt") == "kept");
	BOOST_TEST(!fs::exists(copy.root() / "res/replace.tmp"));
}
#endif

// #910 review: a project whose content roots its own rollback would refuse is not migrated at all.
BOOST_AUTO_TEST_CASE(AMigration_RefusesContentRootsItsRollbackWouldRefuse)
{
	const std::string manifest_text = read_text(copy.root() / "project.snkproj");
	for (const std::string& extra : std::vector<std::string>{ "out", ".content", "code/assets" }) {
		BOOST_TEST_CONTEXT(extra) {
			json::value manifest = json::parse(manifest_text);
			manifest.as_object()["content_roots"] = json::array{ "res", json::string(extra) };
			write_text(copy.root() / "project.snkproj", json::serialize(manifest));
			fs::create_directories(copy.root() / extra);
			const auto before = content_of(copy.root());
			const pmg::migration_plan plan = pmg::plan_migration(copy.root(), pmg::scan_project(copy.root(), *descs), *descs);
			BOOST_TEST(pmg::plan_refusal(plan).find("could not be rolled back") != std::string::npos, pmg::plan_refusal(plan));
			const pmg::apply_outcome outcome = apply(copy.root(), context_for(copy.root()));
			BOOST_TEST(outcome.refused.find("could not be rolled back") != std::string::npos, outcome.refused);
			BOOST_TEST((content_of(copy.root()) == before));
			BOOST_TEST(!fs::exists(copy.root() / ".snk/migrations"));
		}
	}
}

BOOST_AUTO_TEST_CASE(TheProjectTemplate_KeepsMigrationsOutOfGit_ButNotTheAgentPolicy)
{
	std::ifstream in(fs::path{ SNK_EDITOR_DATA_DIR } / "templates/projects/empty/.gitignore");
	std::vector<std::string> lines;
	for (std::string line; std::getline(in, line);) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		lines.push_back(line);
	}
	BOOST_TEST((std::ranges::find(lines, ".snk/*") != lines.end()));
	BOOST_TEST((std::ranges::find(lines, "!.snk/agent_policy.json") != lines.end()));
}

BOOST_AUTO_TEST_SUITE_END()
