#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_apply.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/project/prj_json_text.h>
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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
		pmg::apply_context context{ root, *descs, *rs, {}, {}, {}, {}, nullptr };
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

BOOST_AUTO_TEST_CASE(ANewerOrBrokenFile_RefusesTheMigration_NothingIsWritten)
{
	test_util::fixture_copy full{ "migration_v1" };
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
