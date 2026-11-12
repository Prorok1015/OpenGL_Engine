#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_scan.h>
#include <snk/desc/desc_schema.h>
#include <snk/resource/res_asset_meta.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>

namespace json = boost::json;
namespace fs = std::filesystem;
namespace pmg = prj::migration;

// Scan and plan of a project's migration (#752, EPIC-58 S2) on the fixture of old versions (#750).
namespace {

std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::map<std::string, std::string> files_of(const fs::path& root)
{
	std::map<std::string, std::string> files;
	for (const auto& entry : fs::recursive_directory_iterator(root)) {
		if (entry.is_regular_file()) files[fs::relative(entry.path(), root).generic_string()] = read_text(entry.path());
	}
	return files;
}

pmg::file_class kind_of(const pmg::scan_result& scan, std::string_view path)
{
	const pmg::scanned_file* file = scan.find(path);
	BOOST_REQUIRE_MESSAGE(file != nullptr, std::string{ path });
	return file->kind;
}

const pmg::file_plan* plan_of(const pmg::migration_plan& plan, std::string_view path)
{
	for (const auto& file : plan.files) {
		if (file.path == path) return &file;
	}
	return nullptr;
}

// A small project of its own, for the cases the fixture does not hold.
struct scratch_project
{
	scratch_project()
	{
		static std::atomic<int> counter{ 0 };
		root = fs::temp_directory_path() / std::format("snk_pmg_{}_{}", std::chrono::steady_clock::now().time_since_epoch().count(), ++counter);
		fs::create_directories(root / "res");
		write("project.snkproj", R"({"format_version": 1, "name": "Scratch", "engine_version": "0.1.0"})");
	}

	~scratch_project()
	{
		std::error_code ignored;
		fs::remove_all(root, ignored);
	}

	void write(const std::string& relative, const std::string& text) const
	{
		fs::create_directories((root / relative).parent_path());
		std::ofstream(root / relative, std::ios::binary) << text;
	}

	fs::path root;
};

struct scan_fixture : test_util::scene_env
{
	test_util::fixture_copy copy{ "migration_v1" };
};

}

BOOST_FIXTURE_TEST_SUITE(PmgScanPlanTests, scan_fixture)

BOOST_AUTO_TEST_CASE(EveryClassOfTheFixture_IsFound)
{
	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);

	BOOST_TEST((scan.manifest.kind == pmg::file_class::outdated));
	BOOST_TEST(scan.manifest.engine_version == "0.1.0");
	for (const char* path : { "res/levels/old.desc", "res/worlds/w_v1.desc", "res/worlds/w_v2.desc", "res/worlds/w_v3.desc",
			"res/prefabs/p_v1.prefab.desc", "res/prefabs/p_v2.prefab.desc", "res/prefabs/p_v3.prefab.desc", "res/cameras/cam_v1.desc" }) {
		BOOST_TEST_INFO(path);
		BOOST_TEST((kind_of(scan, path) == pmg::file_class::outdated));
	}
	BOOST_TEST((kind_of(scan, "res/module/unknown.desc") == pmg::file_class::unknown_type));
	BOOST_TEST(scan.find("res/module/unknown.desc")->unknown_types.front() == "migration_fixture_module_desc");
	BOOST_TEST((kind_of(scan, "res/newer.desc") == pmg::file_class::newer));
	BOOST_TEST(!scan.find("res/newer.desc")->reason.empty());
	BOOST_TEST((kind_of(scan, "res/broken.desc") == pmg::file_class::invalid));
	BOOST_TEST((kind_of(scan, "res/textures/plain.png") == pmg::file_class::missing_resource));
	BOOST_TEST((scan.find("README.md") == nullptr)); // not under a content root

	const auto& camera = scan.find("res/cameras/cam_v1.desc")->objects;
	BOOST_REQUIRE(camera.size() == 1u);
	BOOST_TEST(camera[0].type == "camera_desc");
	BOOST_TEST(camera[0].version == 1);
	BOOST_TEST(camera[0].current == descs->current_version("camera_desc"));
}

BOOST_AUTO_TEST_CASE(ThePlan_HasTheStepsTheirSummariesAndWarnings)
{
	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);
	const pmg::migration_plan plan = pmg::plan_migration(copy.root(), scan, *descs);
	BOOST_TEST(!plan.empty());
	BOOST_TEST(plan.files.size() == scan.count(pmg::file_class::outdated));

	const pmg::file_plan* world = plan_of(plan, "res/worlds/w_v3.desc");
	BOOST_REQUIRE(world != nullptr);
	const auto step = std::ranges::find_if(world->steps, [](const auto& s) { return s.type == "world_desc" && s.from_version == 3; });
	BOOST_REQUIRE(step != world->steps.end());
	BOOST_TEST(!step->summary.empty());
	BOOST_REQUIRE(world->warnings.size() == 1u);
	BOOST_TEST(world->warnings[0].type == "world_desc");
	BOOST_TEST(world->warnings[0].file == "res/worlds/w_v3.desc");
	BOOST_TEST(world->warnings[0].message.find("sets no whole legacy camera_desc.transform") != std::string::npos);

	const pmg::file_plan* camera = plan_of(plan, "res/cameras/cam_v1.desc");
	BOOST_REQUIRE(camera != nullptr);
	BOOST_REQUIRE(camera->steps.size() == 1u);
	BOOST_TEST(camera->steps[0].from_version == 1);
	BOOST_TEST(camera->warnings.size() == 1u);

	const pmg::file_plan* old_world = plan_of(plan, "res/worlds/w_v1.desc");
	BOOST_REQUIRE(old_world != nullptr);
	BOOST_TEST(old_world->steps.size() >= 3u); // 1 -> 2 -> 3 -> 4
	BOOST_TEST(old_world->diff.added + old_world->diff.removed + old_world->diff.changed > 0u);
	BOOST_TEST(!old_world->full_diff.has_value());

	const auto total = std::ranges::find_if(plan.steps, [](const auto& s) { return s.type == "prefab_desc" && s.from_version == 1; });
	BOOST_REQUIRE(total != plan.steps.end());
	BOOST_TEST(total->objects >= 1u);
}

BOOST_AUTO_TEST_CASE(TheFullDiff_IsThereOnlyWhenAsked)
{
	const pmg::scan_result scan = pmg::scan_project(copy.root(), *descs);
	pmg::plan_options options;
	options.full_diff = true;
	const pmg::migration_plan plan = pmg::plan_migration(copy.root(), scan, *descs, options);
	const pmg::file_plan* camera = plan_of(plan, "res/cameras/cam_v1.desc");
	BOOST_REQUIRE(camera != nullptr);
	BOOST_REQUIRE(camera->full_diff.has_value());
	BOOST_TEST(camera->full_diff->find("-") != std::string::npos);
	BOOST_TEST(camera->full_diff->find("\"transform\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ScanAndPlan_WriteNothing)
{
	const auto before = files_of(copy.root());
	pmg::plan_options options;
	options.full_diff = true;
	const pmg::migration_plan plan = pmg::plan_migration(copy.root(), pmg::scan_project(copy.root(), *descs), *descs, options);
	BOOST_TEST(!plan.empty());
	BOOST_TEST((files_of(copy.root()) == before));
}

// A module's type, once its layer is there, is known: the file is planned like any other.
BOOST_AUTO_TEST_CASE(ATypeRegisteredInAChildLayer_IsKnown)
{
	desc::desc_system layer(*rs, descs.get());
	layer.register_schema(desc::parse_type_schema(json::parse(R"({"name": "migration_fixture_module_desc", "kind": "component",
		"fields": [{ "name": "hp", "kind": "i32", "default": 0 }]})").as_object()));
	const pmg::scan_result scan = pmg::scan_project(copy.root(), layer);
	BOOST_TEST((kind_of(scan, "res/module/unknown.desc") == pmg::file_class::current));
	BOOST_TEST((kind_of(scan, "res/worlds/w_v1.desc") == pmg::file_class::outdated));
}

BOOST_AUTO_TEST_CASE(ABigFileALinkAndASidecar)
{
	scratch_project project;
	project.write("res/small.desc", R"({"__type":"camera_desc","__version":2})");
	project.write("res/big.desc", std::string(R"({"__type":"camera_desc","__version":2,"pad":")") + std::string(256, 'x') + "\"}");
	project.write("res/tex.png", "png");
	project.write("res/tex.png.resource", res::write_asset_meta(res::asset_meta{ res::generate_asset_id(), "copy", {}, {} }));
	project.write("res/newer.png.resource", R"({"__type": "resource", "__version": 2, "__guid": "x", "future": 1})");
	project.write("res/broken.png.resource", "{}");
	project.write("res/.hidden/skip.desc", "not json");
	std::error_code link_error;
	fs::create_symlink(project.root / "res/small.desc", project.root / "res/link.desc", link_error);

	pmg::scan_options options;
	options.max_file_bytes = 128;
	const pmg::scan_result scan = pmg::scan_project(project.root, *descs, options);
	BOOST_TEST((kind_of(scan, "res/small.desc") == pmg::file_class::current));
	BOOST_TEST((kind_of(scan, "res/big.desc") == pmg::file_class::invalid));
	BOOST_TEST(scan.find("res/big.desc")->reason.find("too big") != std::string::npos);
	BOOST_TEST((kind_of(scan, "res/tex.png") == pmg::file_class::current));
	BOOST_TEST((kind_of(scan, "res/tex.png.resource") == pmg::file_class::current));
	BOOST_TEST((kind_of(scan, "res/newer.png.resource") == pmg::file_class::newer));
	BOOST_TEST((kind_of(scan, "res/broken.png.resource") == pmg::file_class::invalid));
	BOOST_TEST((scan.find("res/.hidden/skip.desc") == nullptr));
	if (!link_error) {
		BOOST_TEST((kind_of(scan, "res/link.desc") == pmg::file_class::invalid));
	} else {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << link_error.message());
	}
	BOOST_TEST(pmg::plan_migration(project.root, scan, *descs).empty());
}

BOOST_AUTO_TEST_CASE(ABadVersion_IsInvalid_ANewerManifest_IsNewer)
{
	scratch_project project;
	project.write("res/bad.desc", R"({"__type":"camera_desc","__version":"two"})");
	const pmg::scan_result scan = pmg::scan_project(project.root, *descs);
	BOOST_TEST((kind_of(scan, "res/bad.desc") == pmg::file_class::invalid));

	project.write("project.snkproj", R"({"format_version": 99, "name": "Scratch", "engine_version": "0.1.0"})");
	const pmg::scan_result newer = pmg::scan_project(project.root, *descs);
	BOOST_TEST((newer.manifest.kind == pmg::file_class::newer));
	BOOST_TEST(newer.files.empty());
}

// The desc loader reads 128 levels; Boost.JSON's default is 32. A deep prefab is not invalid here.
BOOST_AUTO_TEST_CASE(ADeeplyNestedDesc_IsReadAsTheLoaderReadsIt)
{
	scratch_project project;
	std::string deep = R"({"__type":"camera_desc","__version":1,"transform":{"position":[1,2,3],"rotation":[0,0,0]},"pad":)";
	for (int i = 0; i < 60; ++i) deep += R"({"a":)";
	deep += "1";
	for (int i = 0; i < 60; ++i) deep += "}";
	deep += "}";
	project.write("res/deep.desc", deep);
	const pmg::scan_result scan = pmg::scan_project(project.root, *descs);
	BOOST_TEST((kind_of(scan, "res/deep.desc") == pmg::file_class::outdated));
	const pmg::migration_plan plan = pmg::plan_migration(project.root, scan, *descs);
	BOOST_TEST(plan.files.size() == 1u);
}

// One folder that cannot be listed is reported; the files after it are still scanned.
BOOST_AUTO_TEST_CASE(AFolderThatCannotBeListed_IsReported_TheRestIsScanned)
{
	scratch_project project;
	project.write("res/a/locked/x.desc", R"({"__type":"camera_desc","__version":2})");
	project.write("res/z/after.desc", R"({"__type":"camera_desc","__version":1})");
	const fs::path locked = project.root / "res/a/locked";
	std::error_code ec;
	fs::permissions(locked, fs::perms::none, ec);
	std::error_code probe;
	fs::directory_iterator still_readable(locked, probe);
	if (ec || !probe) {
		fs::permissions(locked, fs::perms::owner_all, ec);
		BOOST_TEST_MESSAGE("folder permissions do not stop this user here: nothing to check");
		return;
	}
	const pmg::scan_result scan = pmg::scan_project(project.root, *descs);
	fs::permissions(locked, fs::perms::owner_all, ec);
	BOOST_TEST((kind_of(scan, "res/a/locked") == pmg::file_class::invalid));
	BOOST_TEST((kind_of(scan, "res/z/after.desc") == pmg::file_class::outdated));
}

// A content root that is a link may lead out of the project: it is reported, not followed.
BOOST_AUTO_TEST_CASE(AContentRootThatIsALink_IsNotFollowed)
{
	scratch_project project;
	scratch_project outside;
	outside.write("elsewhere/x.desc", R"({"__type":"camera_desc","__version":1})");
	fs::remove_all(project.root / "res");
	std::error_code link_error;
	fs::create_directory_symlink(outside.root / "elsewhere", project.root / "res", link_error);
	if (link_error) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << link_error.message());
		return;
	}
	const pmg::scan_result scan = pmg::scan_project(project.root, *descs);
	BOOST_TEST((kind_of(scan, "res") == pmg::file_class::invalid));
	BOOST_TEST((scan.find("res/x.desc") == nullptr));
}

BOOST_AUTO_TEST_CASE(AFormatVersionOutOfRange_IsInvalid)
{
	scratch_project project;
	project.write("project.snkproj", R"({"format_version": 18446744073709551615, "name": "Scratch", "engine_version": "0.1.0"})");
	const pmg::scan_result scan = pmg::scan_project(project.root, *descs);
	BOOST_TEST((scan.manifest.kind == pmg::file_class::invalid));
}

BOOST_AUTO_TEST_SUITE_END()
