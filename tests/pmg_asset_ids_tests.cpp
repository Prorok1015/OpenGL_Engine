#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_apply.h>
#include <snk/project_migration/pmg_asset_steps.h>
#include <snk/project_migration/pmg_assets.h>
#include <snk/desc/desc_schema.h>
#include <snk/resource/res_asset_meta.h>
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

// EPIC-57's step assets.ids in the migration of EPIC-58 (#776 PR 1), on copies of the fixture of old versions (#750).
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
		pmg::apply_context context{ copy.root(), *descs, *rs, {}, {}, {}, {}, &registry };
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
// in the report. Neither desc loses its __guid before #772.
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
		const json::value* guid = json::parse(read_text(copy.res() / world)).as_object().if_contains("__guid");
		BOOST_REQUIRE(guid != nullptr);
		BOOST_TEST(guid->as_string() == k_guid);
	}
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

BOOST_AUTO_TEST_SUITE_END()
