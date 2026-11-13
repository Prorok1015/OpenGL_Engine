#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_project_steps.h>
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

	pmg::migration_plan plan()
	{
		pmg::plan_options options;
		options.project_steps = &registry;
		return pmg::plan_migration(copy.root(), pmg::scan_project(copy.root(), *descs), *descs, options);
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
	const std::string original = read_text(copy.root() / "project.snkproj");
	const pmg::migration_plan planned = plan();
	BOOST_TEST(planned.manifest_outdated());
	BOOST_REQUIRE(planned.project_steps.size() == 1u);
	BOOST_TEST(planned.project_steps[0].id == "engine.manifest_format");
	BOOST_TEST(planned.project_steps[0].phase == "pre");

	const pmg::apply_outcome outcome = pmg::apply_migration(planned, context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_TEST((outcome.report.status == pmg::migration_status::partial)); // module/unknown.desc stays as it is
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

// The .resource hook (EPIC-58 3.4): a sidecar's steps are the game layer's migrations of resource_meta.
BOOST_AUTO_TEST_CASE(AResourceMetaStepV1ToV2_MigratesTheSidecar)
{
	const std::string sidecar = "res/textures/with_meta.png.resource";
	BOOST_TEST((pmg::scan_project(copy.root(), *descs).find(sidecar)->kind == pmg::file_class::current));

	resource_meta_steps steps{ *descs };
	const pmg::migration_plan planned = plan();
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

	const json::value meta = json::parse(read_text(copy.root() / sidecar));
	BOOST_TEST(meta.at("__version").as_int64() == 2);
	BOOST_TEST((strings_of(meta.at("flags")) == std::vector<std::string>{ "v2" }));
	BOOST_TEST(meta.at("__guid").as_string() == "5d2f8a41-7c3e-4b9a-8e16-2f0c9b7a4d53");
	BOOST_TEST((pmg::scan_project(copy.root(), *descs).find(sidecar)->kind == pmg::file_class::current));
	BOOST_TEST(plan().empty());
}

// Without the step a version 2 sidecar is newer, as the resource reader sees it; a sidecar of another __type is invalid.
BOOST_AUTO_TEST_CASE(ASidecar_IsClassifiedByTheStepsOfResourceMeta)
{
	const std::string v2 = R"({"__type": "resource_meta", "__version": 2, "__guid": "x"})";
	BOOST_TEST((pmg::classify_sidecar("a.resource", v2, *descs).kind == pmg::file_class::newer));
	BOOST_TEST((pmg::classify_sidecar("a.resource", R"({"__type": "camera_desc"})", *descs).kind == pmg::file_class::invalid));
	BOOST_TEST((pmg::classify_sidecar("a.resource", R"({"__type": "resource_meta", "__version": 1})", *descs).kind == pmg::file_class::invalid));
	{
		resource_meta_steps steps{ *descs };
		BOOST_TEST((pmg::classify_sidecar("a.resource", v2, *descs).kind == pmg::file_class::current));
		BOOST_TEST((pmg::classify_sidecar("a.resource", read_text(copy.res() / "textures/with_meta.png.resource"), *descs).kind
			== pmg::file_class::outdated));
	}
	BOOST_TEST((pmg::classify_sidecar("a.resource", read_text(copy.res() / "textures/with_meta.png.resource"), *descs).kind
		== pmg::file_class::current));
}

BOOST_AUTO_TEST_SUITE_END()
