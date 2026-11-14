#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_asset_steps.h>
#include <snk/project_migration/pmg_assets.h>
#include <snk/desc/desc_schema.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_asset_walk.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <algorithm>
#include <filesystem>
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

// EPIC-57's steps assets.ids (#776 PR 1) and assets.ids_drop_desc_guid (#772) in the migration of EPIC-58, on copies of the
// fixture of old versions (#750).
namespace {

constexpr std::string_view k_guid = "00000000-0000-4000-8000-0000000000b1";

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

std::string with_guid(const std::string& text, std::string_view guid)
{
	json::object root = json::parse(text).as_object();
	json::object out;
	for (const auto& member : root) {
		out[member.key()] = member.value();
		if (member.key() == "__type") out["__guid"] = guid;
	}
	return json::serialize(out);
}

// The ID of a .resource, "unreadable" when it does not read.
std::string id_in(const fs::path& sidecar)
{
	const res::asset_meta_result meta = res::read_asset_meta(read_text(sidecar));
	return meta ? res::to_text(meta.meta->guid) : std::string{ "unreadable" };
}

bool any_note(const pmg::migration_report& report, std::string_view part)
{
	return std::ranges::any_of(report.project_notes, [part](const std::string& note) { return note.find(part) != std::string::npos; });
}

void fails(pmg::project_step_context&)
{
	throw std::runtime_error("injected step failure");
}

struct asset_ids_fixture : test_util::scene_env
{
	asset_ids_fixture()
	{
		fs::remove(copy.res() / "newer.desc");
		fs::remove(copy.res() / "broken.desc");
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ copy.res() });
		pmg::register_project_steps(registry);
		descs->register_schema(desc::parse_type_schema(json::parse(R"({"name": "migration_fixture_module_desc", "kind": "component",
			"fields": [{ "name": "hp", "kind": "i32", "default": 0 }]})").as_object()));
	}

	pmg::migration_plan plan()
	{
		pmg::plan_options options;
		options.project_steps = &registry;
		return pmg::plan_migration(copy.root(), pmg::scan_project(copy.root(), *descs, options.scan), *descs, options);
	}

	pmg::apply_context context()
	{
		pmg::apply_context context{ copy.root(), *descs, *rs, {}, {}, {}, {}, &registry, {}, {} };
		context.make_id = [] { return std::string{ "test-migration" }; };
		return context;
	}

	// Every file the scan lists that is no .resource has one.
	bool every_file_has_a_resource()
	{
		const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);
		return std::ranges::all_of(scan.files, [](const pmg::scanned_file& file) { return file.has_resource; });
	}

	test_util::fixture_copy copy{ "migration_v1" };
	pmg::project_step_registry registry;
};

}

BOOST_FIXTURE_TEST_SUITE(PmgAssetIdsTests, asset_ids_fixture)

BOOST_AUTO_TEST_CASE(EveryContentFile_GetsAResource_TheSecondRunHasNothingToDo)
{
	BOOST_REQUIRE(!every_file_has_a_resource());
	const pmg::migration_plan planned = plan();
	BOOST_REQUIRE((std::ranges::find(planned.project_steps, "assets.ids", &pmg::planned_project_step::id) != planned.project_steps.end()));

	const pmg::apply_outcome outcome = pmg::apply_migration(planned, context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(every_file_has_a_resource());
	const auto& created = outcome.report.created_files;
	BOOST_TEST((std::ranges::find(created, "res/textures/plain.png.resource") != created.end()));
	BOOST_TEST((std::ranges::find(created, "res/worlds/w_v1.desc.resource") != created.end()));
	BOOST_TEST((std::ranges::find(created, "res/textures/with_meta.png.resource") == created.end()));
	BOOST_TEST(id_in(copy.res() / "textures/plain.png.resource") != "unreadable");

	pmg::project_assets assets(copy.root(), { "res" });
	assets.rebuild();
	BOOST_TEST(assets.orphans().empty());
	for (const auto& [path, asset] : assets.assets()) BOOST_TEST(asset.id.has_value(), path);

	BOOST_TEST(plan().empty());
}

// Two copies of one desc carry one __guid: the first by path keeps it as its ID, the other gets a new one, and both are
// in the report. assets.ids_drop_desc_guid then takes the __guid out of both (#772), the copy's with a line of its own.
BOOST_AUTO_TEST_CASE(ADescsGuid_IsItsId_ACopyGetsANewOne_BothAreReported)
{
	write_text(copy.res() / "worlds/w_v1.desc", with_guid(read_text(copy.res() / "worlds/w_v1.desc"), k_guid));
	write_text(copy.res() / "worlds/w_v2.desc", with_guid(read_text(copy.res() / "worlds/w_v2.desc"), k_guid));

	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(id_in(copy.res() / "worlds/w_v1.desc.resource") == k_guid);
	const std::string second = id_in(copy.res() / "worlds/w_v2.desc.resource");
	BOOST_TEST(second != k_guid);
	BOOST_TEST(second != "unreadable");
	BOOST_TEST(any_note(outcome.report, std::string("res/worlds/w_v1.desc: ID ") + std::string(k_guid) + " from its __guid"));
	BOOST_TEST(any_note(outcome.report, std::string("res/worlds/w_v2.desc: its __guid ") + std::string(k_guid) + " is already the ID of res/worlds/w_v1.desc"));
	for (const std::string_view world : { "worlds/w_v1.desc", "worlds/w_v2.desc" }) {
		BOOST_TEST(!json::parse(read_text(copy.res() / world)).as_object().contains("__guid"), world);
	}
	BOOST_TEST(any_note(outcome.report, std::string("res/worlds/w_v2.desc: its __guid \"") + std::string(k_guid) + "\" is taken out; the ID of the file is " + second));
	BOOST_TEST(!any_note(outcome.report, "res/worlds/w_v1.desc: its __guid \""));
}

// #772: after the migration no desc has a __guid; its ID is the .resource, and a rollback puts the __guid back as it was.
BOOST_AUTO_TEST_CASE(TheGuidOfADesc_IsTakenOutAfterItBecameTheId_ARollbackPutsItBack)
{
	write_text(copy.res() / "worlds/w_v1.desc", with_guid(read_text(copy.res() / "worlds/w_v1.desc"), k_guid));
	const auto before = content_of(copy.root());
	const pmg::migration_plan planned = plan();
	BOOST_REQUIRE((std::ranges::find(planned.project_steps, "assets.ids_drop_desc_guid", &pmg::planned_project_step::id) != planned.project_steps.end()));

	const pmg::apply_outcome outcome = pmg::apply_migration(planned, context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(id_in(copy.res() / "worlds/w_v1.desc.resource") == k_guid);
	BOOST_TEST(!json::parse(read_text(copy.res() / "worlds/w_v1.desc")).as_object().contains("__guid"));
	BOOST_TEST(plan().empty());

	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	BOOST_TEST((content_of(copy.root()) == before));
}

// A desc of a lower content root that a higher one covers is not in the index, but has a .resource of its own beside it:
// that is its ID, and its __guid goes too, so the next plan is empty.
BOOST_AUTO_TEST_CASE(ACoveredDescOfALowerRoot_LosesItsGuidToItsOwnResource)
{
	json::object manifest = json::parse(read_text(copy.root() / "project.snkproj")).as_object();
	manifest["content_roots"] = json::array{ "res", "base" };
	write_text(copy.root() / "project.snkproj", json::serialize(manifest));
	res::asset_meta lower;
	lower.guid = *res::asset_id_from_text(k_guid);
	write_text(copy.root() / "base/worlds/w_v1.desc", with_guid(read_text(copy.res() / "worlds/w_v1.desc"), k_guid));
	write_text(copy.root() / "base/worlds/w_v1.desc.resource", res::write_asset_meta(lower));

	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);
	const pmg::scanned_file* covered = scan.find("base/worlds/w_v1.desc");
	BOOST_REQUIRE(covered != nullptr);
	BOOST_TEST(covered->root_guid);
	BOOST_TEST(covered->resource_path == "base/worlds/w_v1.desc.resource");

	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(!json::parse(read_text(copy.root() / "base/worlds/w_v1.desc")).as_object().contains("__guid"));
	BOOST_TEST(id_in(copy.root() / "base/worlds/w_v1.desc.resource") == k_guid);
	BOOST_TEST(plan().empty());
}

// A .resource bigger than the asset index reads (res::asset_meta_max_bytes) is invalid to the scan too, with the reason.
BOOST_AUTO_TEST_CASE(AResourceBiggerThanTheIndexReads_IsInvalidToTheScan)
{
	const std::string text = read_text(copy.res() / "textures/with_meta.png.resource");
	write_text(copy.res() / "textures/with_meta.png.resource", text + std::string(res::asset_meta_max_bytes, ' '));
	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);
	const pmg::scanned_file* sidecar = scan.find("res/textures/with_meta.png.resource");
	BOOST_REQUIRE(sidecar != nullptr);
	BOOST_TEST((sidecar->kind == pmg::file_class::invalid));
	BOOST_TEST(sidecar->reason.find("too big") != std::string::npos, sidecar->reason);
}

// A .resource that does not read (two __guid after a bad merge) has no ID for the desc. After a migration, a desc with a
// __guid and such a .resource does not ask for assets.ids_drop_desc_guid again: the step could not take the __guid out,
// so it would be asked for on every open. (The .resource itself is invalid to the scan, which refuses a migration.)
BOOST_AUTO_TEST_CASE(ADescWithAResourceThatDoesNotRead_DoesNotAskForTheStep)
{
	BOOST_REQUIRE(pmg::apply_migration(plan(), context()).refused.empty());
	BOOST_REQUIRE(plan().empty());
	const std::string id = id_in(copy.res() / "worlds/w_v1.desc.resource");
	write_text(copy.res() / "worlds/w_v1.desc", with_guid(read_text(copy.res() / "worlds/w_v1.desc"), id));
	write_text(copy.res() / "worlds/w_v1.desc.resource", "{\"__type\": \"resource_meta\", \"__version\": 1, \"__guid\": \"" + id
		+ "\", \"__guid\": \"" + std::string(k_guid) + "\", \"importer\": \"\", \"settings\": {}, \"flags\": []}");

	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);
	const pmg::scanned_file* desc = scan.find("res/worlds/w_v1.desc");
	BOOST_REQUIRE(desc != nullptr);
	BOOST_TEST(desc->root_guid);
	BOOST_TEST(desc->has_resource);
	BOOST_TEST(desc->resource_unreadable);
	const pmg::migration_plan planned = plan();
	BOOST_TEST((std::ranges::find(planned.project_steps, "assets.ids_drop_desc_guid", &pmg::planned_project_step::id) == planned.project_steps.end()));
}

// A __guid that an existing .resource already holds is not given twice either.
BOOST_AUTO_TEST_CASE(AGuidAnExistingResourceHolds_GivesANewId)
{
	const std::string taken = id_in(copy.res() / "textures/with_meta.png.resource");
	write_text(copy.res() / "worlds/w_v1.desc", with_guid(read_text(copy.res() / "worlds/w_v1.desc"), taken));
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(id_in(copy.res() / "worlds/w_v1.desc.resource") != taken);
	BOOST_TEST(any_note(outcome.report, "is already the ID of res/textures/with_meta.png"));
}

BOOST_AUTO_TEST_CASE(ARollback_DeletesEveryCreatedResource)
{
	const auto before = content_of(copy.root());
	BOOST_REQUIRE(pmg::apply_migration(plan(), context()).refused.empty());
	BOOST_REQUIRE(every_file_has_a_resource());
	const auto error = pmg::rollback_migration(copy.root(), "test-migration");
	BOOST_REQUIRE_MESSAGE(!error.has_value(), error.value_or(""));
	BOOST_TEST((content_of(copy.root()) == before));
}

BOOST_AUTO_TEST_CASE(AFailedMigration_DeletesEveryCreatedResource)
{
	const auto before = content_of(copy.root());
	pmg::project_step failing;
	failing.id = "test.failing";
	failing.phase = pmg::project_phase::post;
	failing.summary = "fails";
	failing.needed = [](const pmg::scan_result&) { return true; };
	failing.apply = fails;
	registry.add(std::move(failing));
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((outcome.report.status == pmg::migration_status::rolled_back));
	BOOST_TEST(!outcome.report.created_files.empty());
	BOOST_TEST((content_of(copy.root()) == before));
}

// The scan pairs a .resource with its file as the editor's asset database does: another letter case of the suffix is
// still the file's, and nothing is created beside it.
BOOST_AUTO_TEST_CASE(AResourceOfAnotherLetterCase_IsTheFilesOwn)
{
	write_text(copy.res() / "textures/upper.png", "png");
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(k_guid);
	write_text(copy.res() / "textures/upper.png.RESOURCE", res::write_asset_meta(meta));
	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);
	BOOST_REQUIRE(scan.find("res/textures/upper.png") != nullptr);
	BOOST_TEST(scan.find("res/textures/upper.png")->has_resource);
	BOOST_TEST((scan.find("res/textures/upper.png")->kind == pmg::file_class::current));

	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE(outcome.refused.empty());
	BOOST_TEST((std::ranges::find(outcome.report.created_files, "res/textures/upper.png.resource") == outcome.report.created_files.end()));
}

// A file a higher content root covers gets its .resource too: the scan sees every root, so the step would be needed
// forever otherwise.
BOOST_AUTO_TEST_CASE(ACoveredFileOfALowerRoot_GetsAResourceToo)
{
	json::object manifest = json::parse(read_text(copy.root() / "project.snkproj")).as_object();
	manifest["content_roots"] = json::array{ "res", "extra" };
	write_text(copy.root() / "project.snkproj", json::serialize(manifest));
	write_text(copy.root() / "extra/textures/plain.png", "covered");
	write_text(copy.root() / "extra/only.png", "x");

	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_TEST_MESSAGE("error: " << outcome.report.error);
	BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(fs::exists(copy.root() / "extra/textures/plain.png.resource"));
	BOOST_TEST(fs::exists(copy.root() / "extra/only.png.resource"));
	BOOST_TEST(every_file_has_a_resource());
	BOOST_TEST(plan().empty());
}

// The blocker of the review: a file of the higher root takes its .resource from the lower root, as the editor pairs
// them. The plan does not ask for a .resource the step would not make, and one run leaves nothing to do.
BOOST_AUTO_TEST_CASE(AFileWhoseResourceIsInALowerRoot_NeedsNone)
{
	json::object manifest = json::parse(read_text(copy.root() / "project.snkproj")).as_object();
	manifest["content_roots"] = json::array{ "res", "base" };
	write_text(copy.root() / "project.snkproj", json::serialize(manifest));
	write_text(copy.root() / "base/textures/a.png", "lower");
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(k_guid);
	write_text(copy.root() / "base/textures/a.png.resource", res::write_asset_meta(meta));
	write_text(copy.res() / "textures/a.png", "higher");

	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);
	BOOST_REQUIRE(scan.find("res/textures/a.png") != nullptr);
	BOOST_TEST(scan.find("res/textures/a.png")->has_resource);

	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(!fs::exists(copy.res() / "textures/a.png.resource"));
	BOOST_TEST(plan().empty());
}

// The ID of a .resource that a higher root covers is still on the disk: a __guid equal to it is not given again.
BOOST_AUTO_TEST_CASE(AGuidACoveredResourceHolds_GivesANewId)
{
	json::object manifest = json::parse(read_text(copy.root() / "project.snkproj")).as_object();
	manifest["content_roots"] = json::array{ "res", "base" };
	write_text(copy.root() / "project.snkproj", json::serialize(manifest));
	res::asset_meta lower;
	lower.guid = *res::asset_id_from_text(k_guid);
	write_text(copy.root() / "base/textures/s.png", "lower");
	write_text(copy.root() / "base/textures/s.png.resource", res::write_asset_meta(lower));
	res::asset_meta higher;
	higher.guid = res::generate_asset_id();
	write_text(copy.res() / "textures/s.png", "higher");
	write_text(copy.res() / "textures/s.png.resource", res::write_asset_meta(higher));
	write_text(copy.res() / "worlds/w_v1.desc", with_guid(read_text(copy.res() / "worlds/w_v1.desc"), k_guid));

	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
	BOOST_TEST(id_in(copy.res() / "worlds/w_v1.desc.resource") != k_guid);
	BOOST_TEST(any_note(outcome.report, "is already the ID of base/textures/s.png"));
}

// A .resource the step makes is byte for byte the one tools/ci/asset_ids.py --write makes (its BARE_META), so
// `asset_ids.py --check` and res_repo_layout_tests accept it.
BOOST_AUTO_TEST_CASE(AResourceTheStepMakes_IsTheOneTheRepositoryToolWrites)
{
	write_text(copy.res() / "worlds/w_v1.desc", with_guid(read_text(copy.res() / "worlds/w_v1.desc"), k_guid));
	const pmg::apply_outcome outcome = pmg::apply_migration(plan(), context());
	BOOST_REQUIRE_MESSAGE(outcome.refused.empty(), outcome.refused);
	BOOST_REQUIRE((outcome.report.status == pmg::migration_status::ok));
	const std::string expected = std::string("{\n"
		"    \"__type\" : \"resource_meta\",\n"
		"    \"__version\" : 1,\n"
		"    \"__guid\" : \"") + std::string(k_guid) + "\",\n"
		"    \"importer\" : \"\",\n"
		"    \"settings\" : {},\n"
		"    \"flags\" : []\n"
		"}\n";
	BOOST_TEST(read_text(copy.res() / "worlds/w_v1.desc.resource") == expected);
}

// The rule of the ID in a desc is the one tools/ci/asset_ids.py (#768) has: a BOM is skipped. (A desc with a BOM is not
// one the engine loads, so the scan calls it invalid and a migration refuses the project before this step runs.)
BOOST_AUTO_TEST_CASE(TheGuidOfADesc_IsReadAsTheRepositoryToolReadsIt)
{
	const std::string desc = std::string(R"({"__type": "camera_desc", "__guid": ")") + std::string(k_guid) + "\"}";
	BOOST_TEST(res::to_text(*pmg::desc_root_guid(desc)) == k_guid);
	BOOST_TEST(res::to_text(*pmg::desc_root_guid("\xEF\xBB\xBF" + desc)) == k_guid);
	BOOST_TEST(!pmg::desc_root_guid(R"({"__guid": ""})").has_value());
	BOOST_TEST(!pmg::desc_root_guid(R"({"__guid": "00000000-0000-0000-0000-000000000000"})").has_value());
	BOOST_TEST(!pmg::desc_root_guid(R"({"__guid": 7})").has_value());
	BOOST_TEST(!pmg::desc_root_guid("[]").has_value());
	BOOST_TEST(!pmg::desc_root_guid("not json").has_value());
}

BOOST_AUTO_TEST_SUITE_END()
