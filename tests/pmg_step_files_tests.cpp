#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_manual_tasks.hpp"
#include "test_scene_env.hpp"
#include "edt_asset_database.h"
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_assets.h>
#include <snk/project_migration/pmg_project_steps.h>
#include <snk/desc/desc_schema.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/res_tag.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace json = boost::json;
namespace fs = std::filesystem;
namespace pmg = prj::migration;

// What a project step may do with the project's files and assets (#756, EPIC-58 S5), on copies of the fixture of old
// versions (#750); and the migration's asset index against the editor's asset database on the same files.
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

constexpr std::string_view k_plain = "res/textures/plain.png";
constexpr std::string_view k_plain_meta = "res/textures/plain.png.resource";
constexpr std::string_view k_with_meta = "res/textures/with_meta.png";
constexpr std::string_view k_camera = "res/cameras/cam_v1.desc";
constexpr std::string_view k_new_guid = "00000000-0000-4000-8000-0000000000a1";
constexpr std::string_view k_fixture_guid = "5d2f8a41-7c3e-4b9a-8e16-2f0c9b7a4d53";

std::string version_of(const std::string& text)
{
	const json::value desc = json::parse(text);
	const json::value* version = desc.as_object().if_contains("__version");
	return std::to_string(version ? version->as_int64() : 1);
}

std::string id_text(const std::optional<res::asset_id>& id)
{
	return id ? res::to_text(*id) : std::string{ "none" };
}

// `text` with a root __guid, written right after __type.
std::string with_guid(const std::string& text, const json::value& guid)
{
	json::object root = json::parse(text).as_object();
	json::object out;
	for (const auto& member : root) {
		out[member.key()] = member.value();
		if (member.key() == "__type") out["__guid"] = guid;
	}
	return json::serialize(out);
}

// The root __guid of a desc's text, "none" without one.
std::string guid_in(const std::string& text)
{
	const json::value root = json::parse(text);
	const json::value* guid = root.as_object().if_contains("__guid");
	return guid ? std::string(guid->as_string()) : std::string{ "none" };
}

std::string new_meta_text()
{
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(k_new_guid);
	meta.importer = "copy";
	return res::write_asset_meta(meta);
}

pmg::project_step test_step(std::string id, pmg::project_phase phase, void (*apply)(pmg::project_step_context&))
{
	pmg::project_step step;
	step.id = std::move(id);
	step.phase = phase;
	step.summary = "a test step";
	step.needed = [](const pmg::scan_result&) { return true; };
	step.apply = apply;
	return step;
}

struct step_files_fixture : test_util::scene_env
{
	step_files_fixture()
	{
		fs::remove(copy.res() / "newer.desc");
		fs::remove(copy.res() / "broken.desc");
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ copy.res() });
		pmg::register_engine_project_steps(registry);
		// The module of res/module/unknown.desc, loaded: the migration is complete.
		descs->register_schema(desc::parse_type_schema(json::parse(R"({"name": "migration_fixture_module_desc", "kind": "component",
			"fields": [{ "name": "hp", "kind": "i32", "default": 0 }]})").as_object()));
		step_log.clear();
		refusals.clear();
	}

	// What the test steps saw and did, in order: a step is a function, so what it records lives here.
	inline static std::vector<std::string> step_log;
	inline static std::vector<std::string> refusals;

	// As asset_ids of EPIC-57 will: a .resource for a file without one, before the files are migrated.
	static void pre_creates_the_resource(pmg::project_step_context& context)
	{
		step_log.push_back("pre camera v" + version_of(context.files.read(k_camera)));
		step_log.push_back("pre id " + id_text(context.assets.id_of(k_plain)));
		context.files.create(k_plain_meta, new_meta_text());
		context.warn("created a .resource");
	}

	static void post_reads_the_migrated_files(pmg::project_step_context& context)
	{
		step_log.push_back("post camera v" + version_of(context.files.read(k_camera)));
		step_log.push_back("post id " + id_text(context.assets.id_of(k_plain)));
	}

	static void post_writes_and_deletes(pmg::project_step_context& context)
	{
		context.files.write(k_plain, "changed by a step");
		context.files.remove(k_with_meta);
		step_log.push_back("post deleted " + std::string(context.files.exists(k_with_meta) ? "no" : "yes"));
	}

	static void post_fails(pmg::project_step_context&)
	{
		throw std::runtime_error("injected step failure");
	}

	static void pre_tries_what_is_refused(pmg::project_step_context& context)
	{
		const auto refused = [](auto&& call) {
			try {
				call();
			} catch (const std::exception& e) {
				refusals.push_back(e.what());
			}
		};
		refused([&] { context.files.create(k_plain, "over a file"); });
		refused([&] { context.files.write("project.snkproj", "{}"); });
		refused([&] { context.files.write("../outside.desc", "{}"); });
		refused([&] { context.files.create("outside_the_roots.txt", "x"); });
		refused([&] { context.files.create(".snk/x.txt", "x"); });
		refused([&] { context.files.write("res/textures/nothing.png", "x"); });
		refused([&] { context.files.remove("res/textures/nothing.png"); });
		refused([&] { context.files.create(".SNK/x.txt", "x"); });
		refused([&] { context.files.create("PROJECT.SNKPROJ", "{}"); });
		refused([&] { context.files.create("res/textures/draft.tmp", "x"); });
	}

	// The second write of a step spells the path otherwise: still one file, one entry, one backup of the original.
	static void post_writes_one_file_twice(pmg::project_step_context& context)
	{
		context.files.write(k_plain, "first");
		context.files.write("res//textures//plain.png", "second");
	}

	static void pre_drops_the_guid(pmg::project_step_context& context)
	{
		json::object root = json::parse(context.files.read("res/worlds/w_v1.desc")).as_object();
		root.erase("__guid");
		context.files.write("res/worlds/w_v1.desc", json::serialize(root));
	}

	// Deleted, then made again: a change of that file, not a created one.
	static void post_deletes_and_makes_again(pmg::project_step_context& context)
	{
		context.files.remove(k_with_meta);
		context.files.create(k_with_meta, "made again");
	}

	pmg::migration_plan plan()
	{
		pmg::plan_options options;
		options.project_steps = &registry;
		return pmg::plan_migration(copy.root(), pmg::scan_project(copy.root(), *descs, options.scan), *descs, options);
	}

	pmg::apply_context context()
	{
		pmg::apply_context context{ copy.root(), *descs, *rs, {}, {}, {}, {}, &registry, {} };
		context.make_id = [] { return std::string{ "test-migration" }; };
		return context;
	}

	const pmg::applied_file* file_of(const pmg::migration_report& report, std::string_view path) const
	{
		const auto found = std::ranges::find(report.files, path, &pmg::applied_file::path);
		return found == report.files.end() ? nullptr : &*found;
	}

	void expect_the_guid_kept(const json::value& guid)
	{
		write_text(copy.res() / "worlds/w_v1.desc", with_guid(read_text(copy.res() / "worlds/w_v1.desc"), guid));
		const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
		BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
		BOOST_TEST_MESSAGE("error: " << outcome.report.error);
		BOOST_TEST((outcome.report.status == pmg::migration_status::ok));
		const json::value saved = json::parse(read_text(copy.res() / "worlds/w_v1.desc"));
		const json::value* kept = saved.as_object().if_contains("__guid");
		BOOST_REQUIRE(kept != nullptr);
		BOOST_TEST((*kept == guid), json::serialize(*kept));
	}

	test_util::fixture_copy copy{ "migration_v1" };
	pmg::project_step_registry registry;
};

}

BOOST_FIXTURE_TEST_SUITE(PmgStepFilesTests, step_files_fixture)

// pre -> the types' migrations -> post; a .resource made by pre is an asset by the time post runs.
BOOST_AUTO_TEST_CASE(PreRunsBeforeTheTypes_PostAfter_AndSeesWhatPreCreated)
{
	registry.add(test_step("test.post", pmg::project_phase::post, post_reads_the_migrated_files));
	registry.add(test_step("test.pre", pmg::project_phase::pre, pre_creates_the_resource));

	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_TEST((outcome.report.status == pmg::migration_status::ok));
	const std::string current = std::to_string(descs->current_version("camera_desc"));
	BOOST_TEST((step_log == std::vector<std::string>{ "pre camera v1", "pre id none", "post camera v" + current, "post id " + std::string(k_new_guid) }));

	BOOST_TEST((outcome.report.created_files == std::vector<std::string>{ std::string(k_plain_meta) }));
	const json::value journal = json::parse(read_text(copy.root() / ".snk/migrations/test-migration/report.json"));
	BOOST_TEST(journal.at("created_files").as_array().size() == 1u);
	BOOST_TEST(journal.at("created_files").as_array()[0].as_string() == k_plain_meta);
	BOOST_TEST(read_text(copy.root() / k_plain_meta) == new_meta_text());
	BOOST_TEST((std::ranges::find(outcome.report.project_notes, "test.pre: created a .resource") != outcome.report.project_notes.end()));
	// project.snkproj stays the last file of the report.
	BOOST_TEST(outcome.report.files.back().path == "project.snkproj");
}

// A created file goes with a rollback, a written or deleted one comes back: by hand later and at once on a failure.
BOOST_AUTO_TEST_CASE(ARollback_DeletesWhatAStepCreated_AndPutsBackWhatItChangedOrDeleted)
{
	const auto before = content_of(copy.root());
	registry.add(test_step("test.pre", pmg::project_phase::pre, pre_creates_the_resource));
	registry.add(test_step("test.post", pmg::project_phase::post, post_writes_and_deletes));

	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(read_text(copy.root() / k_plain) == "changed by a step");
	BOOST_TEST(!fs::exists(copy.root() / k_with_meta));
	BOOST_TEST((std::ranges::find(step_log, "post deleted yes") != step_log.end()));
	const pmg::applied_file* written = file_of(outcome.report, k_plain);
	BOOST_REQUIRE(written);
	BOOST_TEST(written->written_as == "step:test.post");
	const pmg::applied_file* deleted = file_of(outcome.report, k_with_meta);
	BOOST_REQUIRE(deleted);
	BOOST_TEST(deleted->hash_after == "removed");
	BOOST_TEST(fs::exists(copy.root() / ".snk/migrations/test-migration/backup" / k_with_meta));

	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	BOOST_TEST((content_of(copy.root()) == before));
}

BOOST_AUTO_TEST_CASE(AFailingStep_RollsBackWhatTheStepsBeforeItDid)
{
	const auto before = content_of(copy.root());
	registry.add(test_step("test.pre", pmg::project_phase::pre, pre_creates_the_resource));
	registry.add(test_step("test.post", pmg::project_phase::post, post_writes_and_deletes));
	registry.add(test_step("test.failing", pmg::project_phase::post, post_fails));

	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((outcome.report.status == pmg::migration_status::rolled_back));
	BOOST_TEST(outcome.report.error.find("injected step failure") != std::string::npos);
	BOOST_TEST((content_of(copy.root()) == before));
}

// #851: the engine's save makes a __guid for a res:// desc that has none (desc_adapter_t::serialize); a migration writes
// the one the file had, or none.
BOOST_AUTO_TEST_CASE(AMigratedDesc_KeepsItsGuid_AndGetsNoneItDidNotHave)
{
	write_text(copy.res() / "worlds/w_v1.desc", with_guid(read_text(copy.res() / "worlds/w_v1.desc"), json::string(k_new_guid)));
	BOOST_REQUIRE(read_text(copy.res() / "worlds/w_v2.desc").find("__guid") == std::string::npos);
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_TEST((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(file_of(outcome.report, "res/worlds/w_v1.desc")->written_as == "engine");
	BOOST_TEST(guid_in(read_text(copy.res() / "worlds/w_v1.desc")) == k_new_guid);
	BOOST_TEST(guid_in(read_text(copy.res() / "worlds/w_v2.desc")) == "none");
}

// The case of #851: a pre step takes the __guid away, the type migration that follows must not bring a new one back
// (the engine's save makes none up since #772; assets.ids_drop_desc_guid takes it out after the type migrations).
BOOST_AUTO_TEST_CASE(AGuidAPreStepTookAway_DoesNotComeBack)
{
	write_text(copy.res() / "worlds/w_v1.desc", with_guid(read_text(copy.res() / "worlds/w_v1.desc"), json::string(k_new_guid)));
	registry.add(test_step("test.drop_guid", pmg::project_phase::pre, pre_drops_the_guid));
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_TEST((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(guid_in(read_text(copy.res() / "worlds/w_v1.desc")) == "none");
	BOOST_TEST(json::parse(read_text(copy.res() / "worlds/w_v1.desc")).as_object().at("__version").as_int64()
		== descs->current_version("world_desc"));
}

// An empty __guid is a __guid the file had: it stays as it was, the engine's save makes no new one in its place.
BOOST_AUTO_TEST_CASE(AnEmptyGuid_StaysEmpty)
{
	write_text(copy.res() / "worlds/w_v1.desc", with_guid(read_text(copy.res() / "worlds/w_v1.desc"), ""));
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_TEST((outcome.report.status == pmg::migration_status::ok));
	const json::value saved = json::parse(read_text(copy.res() / "worlds/w_v1.desc"));
	const json::value* guid = saved.as_object().if_contains("__guid");
	BOOST_REQUIRE(guid != nullptr);
	BOOST_TEST(guid->as_string() == "");
}

// A __guid that is no ID is still the file's: the engine's save keeps a string one and makes a new one for any other,
// the migration writes what the file had.
BOOST_AUTO_TEST_CASE(AStringGuidThatIsNoId_StaysAsItWas)
{
	expect_the_guid_kept(json::value("abc"));
}

BOOST_AUTO_TEST_CASE(AGuidThatIsNoString_StaysAsItWas)
{
	expect_the_guid_kept(json::value(7));
}

BOOST_AUTO_TEST_CASE(WhatAStepMayNotTouch_IsRefused_AndChangesNothing)
{
	const auto before = content_of(copy.root());
	registry.add(test_step("test.refused", pmg::project_phase::pre, pre_tries_what_is_refused));
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(refusals.size() == 10u);
	for (const std::string& message : refusals) BOOST_TEST(message.starts_with("test.refused: "), message);
	BOOST_TEST(outcome.report.created_files.empty());
	BOOST_TEST(read_text(copy.root() / k_plain) == before.at(std::string(k_plain)));
	BOOST_TEST(!fs::exists(copy.root() / "outside_the_roots.txt"));
	BOOST_TEST(!fs::exists(copy.root() / ".snk/x.txt"));
	BOOST_TEST(!fs::exists(copy.root() / ".SNK/x.txt"));
	BOOST_TEST(!fs::exists(copy.res() / "textures/draft.tmp"));
}

BOOST_AUTO_TEST_CASE(TwoSpellingsOfOnePath_AreOneFile_WithOneBackupOfTheOriginal)
{
	const std::string original = read_text(copy.root() / k_plain);
	registry.add(test_step("test.twice", pmg::project_phase::post, post_writes_one_file_twice));
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(read_text(copy.root() / k_plain) == "second");
	const auto entries = std::ranges::count_if(outcome.report.files, [](const pmg::applied_file& file) { return file.path.ends_with("plain.png"); });
	BOOST_TEST(entries == 1);
	BOOST_TEST(read_text(copy.root() / ".snk/migrations/test-migration/backup" / k_plain) == original);
}

// A rollback puts the file back from its backup and has nothing to delete: it was never a created file.
BOOST_AUTO_TEST_CASE(AFileDeletedAndMadeAgain_IsAChange_TheRollbackPutsTheOriginalBack)
{
	const auto before = content_of(copy.root());
	registry.add(test_step("test.again", pmg::project_phase::post, post_deletes_and_makes_again));
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(read_text(copy.root() / k_with_meta) == "made again");
	BOOST_TEST(outcome.report.created_files.empty());

	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	BOOST_TEST((content_of(copy.root()) == before));
	// Run again, it finds everything as it was and still succeeds.
	BOOST_TEST(!pmg::rollback_migration(copy.root(), "test-migration").has_value());
}

BOOST_AUTO_TEST_CASE(TheIndex_KnowsTheIdsOfTheResourceFiles_AndTheOrphans)
{
	write_text(copy.res() / "textures/gone.png.resource", new_meta_text());
	write_text(copy.res() / "textures/broken.png", "png");
	write_text(copy.res() / "textures/broken.png.resource", "{}");
	pmg::project_assets assets(copy.root(), { "res" });
	BOOST_TEST(assets.assets().empty());
	assets.rebuild();
	BOOST_TEST(id_text(assets.id_of(k_with_meta)) == k_fixture_guid);
	BOOST_TEST(assets.assets().contains(std::string(k_plain)));
	BOOST_TEST(!assets.id_of(k_plain).has_value());
	BOOST_TEST(!assets.assets().contains(std::string(k_with_meta) + ".resource"));
	BOOST_TEST((assets.paths_of(*res::asset_id_from_text(k_fixture_guid)) == std::vector<std::string>{ std::string(k_with_meta) }));
	BOOST_REQUIRE(assets.orphans().contains("res/textures/gone.png.resource"));
	BOOST_TEST(id_text(assets.orphans().at("res/textures/gone.png.resource").id) == k_new_guid);
	BOOST_TEST(!assets.id_of("res/textures/broken.png").has_value());
	const auto names = [&](std::string_view part) {
		return std::ranges::any_of(assets.problems(), [part](const std::string& line) { return line.find(part) != std::string::npos; });
	};
	BOOST_TEST(names("res/textures/gone.png.resource"));
	BOOST_TEST(names("res/textures/broken.png.resource"));
}

// The condition of #756: the migration's index lists what the editor's asset database lists, by the same rules. The
// editor stacks the content roots as res:// does, the first root highest.
void expect_the_editors_view(const fs::path& root, const std::vector<fs::path>& content_roots)
{
	pmg::project_assets assets(root, content_roots);
	assets.rebuild();

	std::vector<fs::path> layers;
	for (auto it = content_roots.rbegin(); it != content_roots.rend(); ++it) layers.push_back((root / *it).lexically_normal());
	ds::task_system tasks{ test_util::manual_tasks() };
	edt::editor_events events;
	edt::job_service jobs{ tasks, &events };
	edt::asset_database database(jobs, &events, layers);
	database.rebuild();
	test_util::pump(tasks);
	BOOST_REQUIRE(!database.scanning());

	std::set<std::string> from_index;
	for (const auto& [path, asset] : assets.assets()) from_index.insert(res::tag::make(asset.res_path).string() + " " + id_text(asset.id));
	// The editor walks a content root "." whole; the index leaves out the project's code, build output and manifest,
	// as the scan does.
	const bool project_root = content_roots.size() == 1 && content_roots.front() == ".";
	const auto own = [project_root](const res::tag& tag) {
		if (!project_root) return false;
		const std::string path(tag.relative());
		return res::is_project_own_name(std::string_view(path).substr(0, path.find('/')));
	};
	std::set<std::string> from_database;
	for (const auto& [key, record] : database.records()) {
		if (!own(record.path)) from_database.insert(record.path.string() + " " + id_text(record.id));
	}
	BOOST_TEST((from_index == from_database));
	BOOST_TEST(!from_index.empty());

	std::set<std::string> index_orphans;
	for (const auto& [path, meta] : assets.orphans()) index_orphans.insert(res::tag::make(meta.res_path).string() + " " + id_text(meta.id));
	std::set<std::string> database_orphans;
	for (const edt::asset_orphan& orphan : database.orphans()) {
		if (!own(orphan.meta_path)) database_orphans.insert(orphan.meta_path.string() + " " + id_text(orphan.id));
	}
	BOOST_TEST((index_orphans == database_orphans));
}

BOOST_AUTO_TEST_CASE(TheIndex_ListsWhatTheEditorsAssetDatabaseLists)
{
	write_text(copy.res() / "Thumbs.db", "x");
	write_text(copy.res() / "textures/.gitkeep", "");
	write_text(copy.res() / "textures/draft.tmp", "x");
	write_text(copy.res() / "textures/draft.tmp.resource", new_meta_text());
	write_text(copy.res() / "textures/x~", "x");
	write_text(copy.res() / ".hidden/inside.png", "x");
	write_text(copy.res() / "deep/a/b/c/leaf.png", "x");
	write_text(copy.res() / "textures/gone.png.resource", new_meta_text());
	// Two sidecars of one file where letter case counts: one pairs, the other is an orphan in both.
	write_text(copy.res() / "textures/dup.png", "png");
	write_text(copy.res() / "textures/dup.png.resource", new_meta_text());
	write_text(copy.res() / "textures/dup.png.RESOURCE", new_meta_text());
	std::error_code link_error;
	fs::create_symlink(copy.res() / "textures/plain.png", copy.res() / "textures/link.png", link_error);

	expect_the_editors_view(copy.root(), { "res" });

	pmg::project_assets assets(copy.root(), { "res" });
	assets.rebuild();
	BOOST_TEST(assets.assets().contains("res/deep/a/b/c/leaf.png"));
	BOOST_TEST(!assets.assets().contains("res/Thumbs.db"));
	BOOST_TEST(!assets.assets().contains("res/textures/draft.tmp"));
	BOOST_TEST(!assets.orphans().contains("res/textures/draft.tmp.resource"));
	BOOST_TEST(!assets.assets().contains("res/.hidden/inside.png"));
	BOOST_TEST(!assets.assets().contains("res/textures/link.png"));
	BOOST_TEST(assets.orphans().contains("res/textures/gone.png.resource"));
	if constexpr (!res::asset_names_case_insensitive) {
		BOOST_TEST((assets.orphans().contains("res/textures/dup.png.resource") || assets.orphans().contains("res/textures/dup.png.RESOURCE")));
	}
}

// A file of the first root covers the same name in the second, as res:// and the editor do: neither lists it twice.
BOOST_AUTO_TEST_CASE(TwoContentRoots_AreStackedAsTheEditorStacksThem)
{
	write_text(copy.root() / "extra/textures/plain.png", "covered");
	write_text(copy.root() / "extra/textures/plain.png.resource", new_meta_text());
	write_text(copy.root() / "extra/only.png", "x");
	write_text(copy.root() / "extra/lost.png.resource", new_meta_text());

	expect_the_editors_view(copy.root(), { "res", "extra" });

	pmg::project_assets assets(copy.root(), { "res", "extra" });
	assets.rebuild();
	BOOST_TEST(assets.assets().contains(std::string(k_plain)));
	BOOST_TEST(!assets.assets().contains("extra/textures/plain.png"));
	BOOST_TEST(assets.assets().contains("extra/only.png"));
	BOOST_TEST(assets.orphans().contains("extra/lost.png.resource"));
}

BOOST_AUTO_TEST_CASE(TheProjectFolderAsItsContentRoot_IsWalkedAsTheEditorWalksIt)
{
	write_text(copy.root() / "code/game.png", "x");
	write_text(copy.root() / "build/out.png", "x");
	write_text(copy.root() / "build/lost.png.resource", new_meta_text());
	expect_the_editors_view(copy.root(), { "." });
	pmg::project_assets assets(copy.root(), { "." });
	assets.rebuild();
	BOOST_TEST(!assets.assets().contains("code/game.png"));
	BOOST_TEST(!assets.assets().contains("build/out.png"));
	BOOST_TEST(!assets.assets().contains("project.snkproj"));
	BOOST_TEST(!assets.orphans().contains("build/lost.png.resource"));
	BOOST_TEST(assets.assets().contains(std::string(k_plain)));
	BOOST_TEST(assets.assets().at(std::string(k_plain)).res_path == "res/textures/plain.png");
}

BOOST_AUTO_TEST_SUITE_END()
