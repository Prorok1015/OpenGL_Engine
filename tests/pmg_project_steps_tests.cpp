#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_project_steps.h>
#include <snk/desc/desc_schema.h>
#include <snk/project/prj_json_text.h>
#include <snk/project/prj_manifest.h>
#include <snk/project/prj_version.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace json = boost::json;
namespace fs = std::filesystem;
namespace pmg = prj::migration;

// Project steps, the stamp of project.snkproj and the .resource hook (#754, EPIC-58 S4) on copies of the fixture of old
// versions (#750).
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

std::vector<std::string> strings_of(const json::value& array)
{
	std::vector<std::string> out;
	for (const json::value& item : array.as_array()) out.emplace_back(item.as_string());
	return out;
}

// What the test steps saw: the version of a camera file on the disk when they ran.
std::vector<std::string> g_seen;
fs::path g_camera;

std::string camera_version()
{
	const json::value camera = json::parse(read_text(g_camera));
	const json::value* version = camera.as_object().if_contains("__version");
	return std::to_string(version ? version->as_int64() : 1);
}

bool not_applied(const pmg::scan_result& scan, std::string_view id)
{
	return std::ranges::find(scan.manifest.applied_steps, id) == scan.manifest.applied_steps.end();
}

pmg::project_step test_step(std::string id, pmg::project_phase phase, void (*apply)(pmg::project_step_context&))
{
	pmg::project_step step;
	step.id = std::move(id);
	step.phase = phase;
	step.summary = "a test step";
	step.apply = apply;
	return step;
}

struct steps_fixture : test_util::scene_env
{
	steps_fixture()
	{
		// The fixture without its newer and broken files, which a migration refuses.
		fs::remove(copy.res() / "newer.desc");
		fs::remove(copy.res() / "broken.desc");
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ copy.res() });
		pmg::register_engine_project_steps(registry);
		g_seen.clear();
		g_camera = copy.res() / "cameras/cam_v1.desc";
	}

	pmg::migration_plan plan(std::int64_t resource_meta_version = res::asset_meta_version)
	{
		pmg::plan_options options;
		options.project_steps = &registry;
		options.scan.resource_meta_version = resource_meta_version;
		return pmg::plan_migration(copy.root(), pmg::scan_project(copy.root(), *descs, options.scan), *descs, options);
	}

	// The module of res/module/unknown.desc, loaded: nothing of the project stays behind.
	void load_the_module()
	{
		descs->register_schema(desc::parse_type_schema(json::parse(R"({"name": "migration_fixture_module_desc", "kind": "component",
			"fields": [{ "name": "hp", "kind": "i32", "default": 0 }]})").as_object()));
	}

	pmg::apply_context context()
	{
		pmg::apply_context context{ copy.root(), *descs, *rs, {}, {}, {}, {}, &registry };
		context.make_id = [] { return std::string{ "test-migration" }; };
		return context;
	}

	json::object manifest() const
	{
		return json::parse(read_text(copy.root() / "project.snkproj")).as_object();
	}

	test_util::fixture_copy copy{ "migration_v1" };
	pmg::project_step_registry registry;
};

// Steps of resource_meta registered for one test only: the game layer outlives no test, but a failed check must not
// leave them behind for the rest of it.
struct resource_meta_steps
{
	explicit resource_meta_steps(desc::desc_system& descs) : m_descs(descs)
	{
		m_descs.register_migration(res::asset_meta_type, 1, "adds the flag v2 (test)",
			[](json::object& meta, desc::migration_context&) { meta["flags"].as_array().emplace_back("v2"); });
	}
	~resource_meta_steps() { m_descs.unregister_migrations(res::asset_meta_type); }
	resource_meta_steps(const resource_meta_steps&) = delete;
	resource_meta_steps& operator=(const resource_meta_steps&) = delete;

private:
	desc::desc_system& m_descs;
};

}

BOOST_FIXTURE_TEST_SUITE(PmgProjectStepsTests, steps_fixture)

BOOST_AUTO_TEST_CASE(TheOldProject_OpensWithoutTheVersionDialog_AfterApply)
{
	load_the_module();
	const std::string original = read_text(copy.root() / "project.snkproj");
	const pmg::migration_plan planned = plan();
	BOOST_TEST(planned.manifest_outdated());
	BOOST_REQUIRE(planned.project_steps.size() == 1u);
	BOOST_TEST(planned.project_steps[0].id == "engine.manifest_format");
	BOOST_TEST(planned.project_steps[0].phase == "pre");

	const pmg::apply_outcome outcome = pmg::apply_migration(planned, context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_TEST((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST((outcome.report.project_steps == std::vector<std::string>{ "engine.manifest_format" }));

	const prj::manifest_result loaded = prj::load_manifest(copy.root());
	BOOST_REQUIRE(loaded.data.has_value());
	BOOST_TEST(loaded.warnings.empty());
	BOOST_TEST(loaded.data->format_version == prj::k_manifest_format_version);
	// The dialog asks when the project's MAJOR.MINOR differs from the engine's (prj_version.cpp).
	BOOST_TEST((prj::compare_engine_versions(loaded.data->engine, prj::current_engine_version()) == prj::version_match::exact));
	const json::object stamped = manifest();
	const std::string engine = prj::to_string(prj::current_engine_version());
	BOOST_TEST(stamped.at("migrated_with").as_string() == engine);
	BOOST_TEST((strings_of(stamped.at("applied_steps")) == std::vector<std::string>{ "engine.manifest_format" }));
	BOOST_TEST(stamped.at("start_scene").as_string() == "res://levels/old.desc");

	BOOST_TEST(read_text(copy.root() / ".snk/migrations/test-migration/backup/project.snkproj") == original);
	const auto file = std::ranges::find_if(outcome.report.files, [](const auto& f) { return f.path == "project.snkproj"; });
	BOOST_REQUIRE(file != outcome.report.files.end());
	BOOST_TEST(file->written_as == "manifest");
	const json::value journal = json::parse(read_text(copy.root() / ".snk/migrations/test-migration/report.json"));
	BOOST_TEST((strings_of(journal.at("project_steps")) == std::vector<std::string>{ "engine.manifest_format" }));
	BOOST_TEST(read_text(copy.root() / ".snk/migrations/test-migration/report.md").find("engine.manifest_format") != std::string::npos);
}

// A file of a module that is not loaded stays as it is: the project keeps its old engine_version, so the dialog still
// asks, but it records the migration and the steps that ran.
BOOST_AUTO_TEST_CASE(APartialMigration_KeepsTheOldEngineVersion_AndTheDialog)
{
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST((outcome.report.status == pmg::migration_status::partial));
	const json::object stamped = manifest();
	BOOST_TEST(stamped.at("engine_version").as_string() == "0.1.0");
	BOOST_TEST(stamped.at("migrated_with").as_string() == prj::to_string(prj::current_engine_version()));
	BOOST_TEST((strings_of(stamped.at("applied_steps")) == std::vector<std::string>{ "engine.manifest_format" }));
	const prj::manifest_result loaded = prj::load_manifest(copy.root());
	BOOST_REQUIRE(loaded.data.has_value());
	BOOST_TEST((prj::compare_engine_versions(loaded.data->engine, prj::current_engine_version()) == prj::version_match::project_older));

	// Until the module is loaded there is nothing more a migration can do.
	BOOST_TEST(plan().empty());
	load_the_module();
	const pmg::migration_plan with_module = plan();
	BOOST_TEST(!with_module.empty());
	BOOST_REQUIRE(pmg::apply_migration(with_module, context()).refused.empty());
	BOOST_TEST(manifest().at("engine_version").as_string() == prj::to_string(prj::current_engine_version()));
}

BOOST_AUTO_TEST_CASE(ASecondRun_HasNothingToDo_AndWritesNothing)
{
	BOOST_REQUIRE(pmg::apply_migration(plan(), context()).refused.empty());
	const auto after = content_of(copy.root());

	const pmg::migration_plan again = plan();
	BOOST_TEST(again.empty());
	BOOST_TEST(!again.manifest_outdated());
	BOOST_TEST(again.project_steps.empty());
	pmg::apply_context second = context();
	second.make_id = [] { return std::string{ "second" }; };
	const pmg::apply_outcome outcome = pmg::apply_migration(again, second);
	BOOST_TEST(outcome.refused.empty());
	BOOST_TEST(outcome.report.id.empty());
	BOOST_TEST((content_of(copy.root()) == after));
	BOOST_TEST(!fs::exists(copy.root() / ".snk/migrations/second"));
}

// Pre steps see the files as they were, post steps as migrated; applied_steps keeps what earlier migrations ran.
BOOST_AUTO_TEST_CASE(PreRunsBeforeTheFiles_PostAfter_AppliedStepsIsAUnion)
{
	json::object old_manifest = manifest();
	old_manifest["applied_steps"] = json::array{ "module.earlier" };
	write_text(copy.root() / "project.snkproj", json::serialize(old_manifest));
	pmg::project_step pre = test_step("test.pre", pmg::project_phase::pre, [](pmg::project_step_context& context) {
		g_seen.push_back("pre:" + camera_version());
		context.notes.push_back("test.pre ran");
	});
	pre.needed = [](const pmg::scan_result& scan) { return not_applied(scan, "test.pre"); };
	pmg::project_step post = test_step("test.post", pmg::project_phase::post, [](pmg::project_step_context&) { g_seen.push_back("post:" + camera_version()); });
	post.needed = [](const pmg::scan_result& scan) { return not_applied(scan, "test.post"); };
	// Registered post first: the phase orders the steps, the registry orders a phase.
	registry.add(std::move(post));
	registry.add(std::move(pre));

	const pmg::migration_plan planned = plan();
	std::vector<std::string> ids;
	for (const auto& step : planned.project_steps) ids.push_back(step.id);
	BOOST_TEST((ids == std::vector<std::string>{ "engine.manifest_format", "test.pre", "test.post" }));

	const pmg::apply_outcome outcome = pmg::apply_migration(planned, context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	const std::string current = std::to_string(descs->current_version("camera_desc"));
	BOOST_TEST((g_seen == std::vector<std::string>{ "pre:1", "post:" + current }));
	BOOST_TEST((strings_of(manifest().at("applied_steps")) == std::vector<std::string>{ "module.earlier", "engine.manifest_format", "test.pre", "test.post" }));
	BOOST_TEST((std::ranges::find(outcome.report.project_notes, "test.pre ran") != outcome.report.project_notes.end()));

	// A step that ran is not needed any more: the hint of applied_steps makes the second run empty.
	BOOST_TEST(plan().empty());
}

BOOST_AUTO_TEST_CASE(AFailingPostStep_RollsBack_TheManifestStaysAsItWas)
{
	const auto before = content_of(copy.root());
	pmg::project_step failing = test_step("test.failing", pmg::project_phase::post, [](pmg::project_step_context&) {
		throw std::runtime_error("injected step failure");
	});
	failing.needed = [](const pmg::scan_result&) { return true; };
	registry.add(std::move(failing));

	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((outcome.report.status == pmg::migration_status::rolled_back));
	BOOST_TEST(outcome.report.error.find("injected step failure") != std::string::npos);
	BOOST_TEST((content_of(copy.root()) == before));
}

// A rollback puts project.snkproj back with the rest: the steps of the migration leave applied_steps with it.
BOOST_AUTO_TEST_CASE(ARollback_TakesTheStepsOutOfTheManifest)
{
	const auto before = content_of(copy.root());
	BOOST_REQUIRE(pmg::apply_migration(plan(), context()).refused.empty());
	BOOST_REQUIRE(manifest().contains("applied_steps"));

	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	BOOST_TEST((content_of(copy.root()) == before));
	BOOST_TEST(!manifest().contains("applied_steps"));
	BOOST_TEST(plan().project_steps.size() == 1u);
}

// project.snkproj changed by hand after the migration: the other files go back as usual, the manifest loses only the
// stamp of the migration and keeps the edit.
BOOST_AUTO_TEST_CASE(ARollback_AfterAnEditOfTheManifest_TakesBackOnlyTheStamp)
{
	const auto before = content_of(copy.root());
	BOOST_REQUIRE(pmg::apply_migration(plan(), context()).refused.empty());
	json::object edited = manifest();
	edited["name"] = "Renamed by hand";
	edited["platforms"] = json::array{ "windows-x64" };
	write_text(copy.root() / "project.snkproj", prj::to_indented_json(edited));

	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	auto after = content_of(copy.root());
	BOOST_TEST(after.at("res/cameras/cam_v1.desc") == before.at("res/cameras/cam_v1.desc"));
	after.erase("project.snkproj");
	auto expected = before;
	expected.erase("project.snkproj");
	BOOST_TEST((after == expected));

	const json::object rolled_back = manifest();
	BOOST_TEST(rolled_back.at("name").as_string() == "Renamed by hand");
	BOOST_TEST(rolled_back.contains("platforms"));
	BOOST_TEST(rolled_back.at("engine_version").as_string() == "0.1.0");
	BOOST_TEST(rolled_back.at("format_version").as_int64() == 1);
	BOOST_TEST(!rolled_back.contains("migrated_with"));
	BOOST_TEST(!rolled_back.contains("applied_steps"));
}

// The manifest is written last: a failed write rolls the files back, and a rollback that stopped half way finishes later.
BOOST_AUTO_TEST_CASE(AFailedWriteOfTheManifest_RollsBack_AndAStoppedRollbackFinishesLater)
{
	const auto before = content_of(copy.root());
	pmg::apply_context failing = context();
	std::map<std::string, int> writes;
	failing.write = [&writes](const fs::path& root, const fs::path& relative, std::string_view text) -> std::optional<std::string> {
		const std::string path = relative.generic_string();
		const int count = ++writes[path];
		if (path == "project.snkproj") return std::string{ "injected manifest failure" };
		if (path == "res/cameras/cam_v1.desc" && count == 2) return std::string{ "injected restore failure" };
		return prj::write_project_file(root, relative, text);
	};
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), failing);
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((outcome.report.status == pmg::migration_status::rollback_failed));
	BOOST_TEST(outcome.report.error.find("injected manifest failure") != std::string::npos);
	BOOST_TEST((pmg::unfinished_migrations(copy.root()) == std::vector<std::string>{ "test-migration" }));

	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	BOOST_TEST((content_of(copy.root()) == before));
	BOOST_TEST(pmg::unfinished_migrations(copy.root()).empty());
}

BOOST_AUTO_TEST_CASE(AKeyOfTheManifestNobodyKnows_SurvivesTheMigration)
{
	json::object old_manifest = manifest();
	old_manifest["studio_note"] = json::object{ { "owner", "art" } };
	write_text(copy.root() / "project.snkproj", json::serialize(old_manifest));
	BOOST_REQUIRE(pmg::apply_migration(plan(), context()).refused.empty());
	BOOST_TEST(manifest().at("studio_note").at("owner").as_string() == "art");
}

BOOST_AUTO_TEST_CASE(AMalformedAppliedSteps_IsReplaced_AndTheReportSaysSo)
{
	json::object old_manifest = manifest();
	old_manifest["applied_steps"] = "engine.manifest_format";
	write_text(copy.root() / "project.snkproj", json::serialize(old_manifest));
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((strings_of(manifest().at("applied_steps")) == std::vector<std::string>{ "engine.manifest_format" }));
	const auto note = std::ranges::find_if(outcome.report.project_notes, [](const std::string& n) { return n.find("applied_steps was not a list") != std::string::npos; });
	BOOST_TEST((note != outcome.report.project_notes.end()));

	old_manifest = manifest();
	old_manifest["applied_steps"] = json::array{ "engine.manifest_format", 7 };
	write_text(copy.root() / "project.snkproj", json::serialize(old_manifest));
	load_the_module();
	pmg::apply_context second = context();
	second.make_id = [] { return std::string{ "second" }; };
	const pmg::apply_outcome again = pmg::apply_migration(plan(), second);
	BOOST_REQUIRE(again.refused.empty());
	BOOST_TEST((strings_of(manifest().at("applied_steps")) == std::vector<std::string>{ "engine.manifest_format" }));
	const auto dropped = std::ranges::find_if(again.report.project_notes, [](const std::string& n) { return n.find("not a string was dropped") != std::string::npos; });
	BOOST_TEST((dropped != again.report.project_notes.end()));
}

BOOST_AUTO_TEST_CASE(AStepTheApplyCannotFind_RefusesTheMigration_NothingIsWritten)
{
	const auto before = content_of(copy.root());
	pmg::apply_context without = context();
	without.project_steps = nullptr;
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), without);
	BOOST_TEST(outcome.refused.find("engine.manifest_format") != std::string::npos);
	BOOST_TEST((content_of(copy.root()) == before));
	BOOST_TEST(!fs::exists(copy.root() / ".snk"));
}

BOOST_AUTO_TEST_CASE(TheRegistry_RefusesATakenIdOrAStepWithoutFunctions)
{
	BOOST_CHECK_THROW(pmg::register_engine_project_steps(registry), std::invalid_argument);
	BOOST_CHECK_THROW(registry.add(test_step("test.no_needed", pmg::project_phase::pre, [](pmg::project_step_context&) {})), std::invalid_argument);
	BOOST_TEST(registry.find("engine.manifest_format") != nullptr);
	BOOST_TEST(registry.find("test.no_needed") == nullptr);
}

// The .resource hook (EPIC-58 3.4): a sidecar's steps are the game layer's migrations of resource_meta. The test reads
// as an engine whose res::asset_meta_version was raised with the step (scan_options::resource_meta_version).
BOOST_AUTO_TEST_CASE(AResourceMetaStepV1ToV2_MigratesTheSidecar_AndTheEngineReadsIt)
{
	const std::string sidecar = "res/textures/with_meta.png.resource";
	BOOST_TEST((pmg::scan_project(copy.root(), *descs).find(sidecar)->kind == pmg::file_class::current));

	resource_meta_steps steps{ *descs };
	const pmg::migration_plan planned = plan(2);
	BOOST_TEST(planned.refused.empty());
	const auto file = std::ranges::find_if(planned.files, [&](const auto& f) { return f.path == sidecar; });
	BOOST_REQUIRE(file != planned.files.end());
	BOOST_REQUIRE(file->steps.size() == 1u);
	BOOST_TEST(file->steps[0].type == res::asset_meta_type);
	BOOST_TEST(file->steps[0].from_version == 1);
	BOOST_TEST(file->steps[0].summary == "adds the flag v2 (test)");

	const pmg::apply_outcome outcome = pmg::apply_migration(planned, context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_TEST((outcome.report.status == pmg::migration_status::partial));
	const auto written = std::ranges::find_if(outcome.report.files, [&](const auto& f) { return f.path == sidecar; });
	BOOST_REQUIRE(written != outcome.report.files.end());
	BOOST_TEST(written->written_as == "json");

	const std::string text = read_text(copy.root() / sidecar);
	const json::value meta = json::parse(text);
	BOOST_TEST(meta.at("__version").as_int64() == 2);
	BOOST_TEST((strings_of(meta.at("flags")) == std::vector<std::string>{ "v2" }));
	BOOST_TEST(meta.at("__guid").as_string() == "5d2f8a41-7c3e-4b9a-8e16-2f0c9b7a4d53");
	BOOST_TEST((pmg::classify_sidecar(sidecar, text, *descs, 2).kind == pmg::file_class::current));
	BOOST_TEST(plan(2).empty());
}

// A step of resource_meta that does not raise the version the engine reads: every migrated sidecar would be newer to the
// asset database, so the plan is refused and nothing is written.
BOOST_AUTO_TEST_CASE(AResourceMetaStep_WithoutTheEngineReadingItsVersion_IsRefused)
{
	const auto before = content_of(copy.root());
	resource_meta_steps steps{ *descs };
	const pmg::migration_plan planned = plan();
	BOOST_TEST(planned.refused.find("res::asset_meta_version") != std::string::npos);
	const pmg::apply_outcome outcome = pmg::apply_migration(planned, context());
	BOOST_TEST(!outcome.refused.empty());
	BOOST_TEST((content_of(copy.root()) == before));
	BOOST_TEST(!fs::exists(copy.root() / ".snk"));
}

BOOST_AUTO_TEST_CASE(ASidecar_IsClassifiedByTheStepsOfResourceMeta_AndReadByTheEngine)
{
	const std::string v1 = read_text(copy.res() / "textures/with_meta.png.resource");
	const std::string v2 = R"({"__type": "resource_meta", "__version": 2, "__guid": "5d2f8a41-7c3e-4b9a-8e16-2f0c9b7a4d53",
		"importer": "copy", "settings": {}, "flags": []})";
	BOOST_TEST((pmg::classify_sidecar("a.resource", v1, *descs).kind == pmg::file_class::current));
	BOOST_TEST((pmg::classify_sidecar("a.resource", v2, *descs).kind == pmg::file_class::newer));
	BOOST_TEST((pmg::classify_sidecar("a.resource", R"({"__type": "camera_desc"})", *descs).kind == pmg::file_class::invalid));
	BOOST_TEST((pmg::classify_sidecar("a.resource", R"({"__type": "resource_meta", "__version": 1})", *descs).kind == pmg::file_class::invalid));
	{
		resource_meta_steps steps{ *descs };
		BOOST_TEST((pmg::classify_sidecar("a.resource", v2, *descs, 2).kind == pmg::file_class::current));
		BOOST_TEST((pmg::classify_sidecar("a.resource", v1, *descs, 2).kind == pmg::file_class::outdated));
		// The steps call it current, but this engine does not read it.
		const pmg::scanned_file unread = pmg::classify_sidecar("a.resource", v2, *descs);
		BOOST_TEST((unread.kind == pmg::file_class::newer));
		BOOST_TEST(!unread.reason.empty());
	}
	BOOST_TEST((pmg::classify_sidecar("a.resource", v1, *descs).kind == pmg::file_class::current));
}

// resource_meta is a type only at the root of a sidecar; what is under its "settings" belongs to the importer.
BOOST_AUTO_TEST_CASE(ResourceMeta_IsKnownAtTheRootOfASidecarOnly_ItsSettingsAreLeftAlone)
{
	const pmg::scanned_file desc_file = pmg::classify_desc("a.desc", R"({"__type": "camera_desc", "__version": 2, "inner": {"__type": "resource_meta"}})", *descs);
	BOOST_TEST((desc_file.kind == pmg::file_class::unknown_type));

	const std::string text = R"({"__type": "resource_meta", "__version": 1, "__guid": "5d2f8a41-7c3e-4b9a-8e16-2f0c9b7a4d53",
		"importer": "copy", "settings": {"inner": {"__type": "camera_desc", "fov": 1}}, "flags": []})";
	BOOST_TEST(pmg::classify_sidecar("a.resource", text, *descs).objects.size() == 1u);

	resource_meta_steps steps{ *descs };
	json::value root = json::parse(text);
	pmg::migrate_sidecar(*descs, root.as_object(), "a.resource");
	BOOST_TEST(root.at("__version").as_int64() == 2);
	const json::object& inner = root.at("settings").at("inner").as_object();
	BOOST_TEST(inner.at("__type").as_string() == "camera_desc");
	BOOST_TEST(!inner.contains("__version"));
	BOOST_TEST(inner.size() == 2u);
}

BOOST_AUTO_TEST_SUITE_END()
