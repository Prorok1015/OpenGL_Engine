#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project/prj_manifest.h>
#include <snk/project/prj_version.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>

namespace json = boost::json;
namespace fs = std::filesystem;

// The project of old versions every migration story tests against (#750, EPIC-58 S0): what each file is today, before
// any migration command exists. tests/data/projects/migration_v1/README.md lists the files.
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

struct migration_fixture : test_util::scene_env
{
	migration_fixture() : before(files_of(test_util::fixture_source("migration_v1"))) {}

	~migration_fixture()
	{
		// A test works on its copy: the fixture in the repository never changes.
		BOOST_CHECK(files_of(test_util::fixture_source("migration_v1")) == before);
	}

	json::object migrated(const std::string& relative)
	{
		auto obj = json::parse(read_text(copy.res() / relative)).as_object();
		descs->migrate_tree(obj, relative);
		return obj;
	}

	std::map<std::string, std::string> before;
	test_util::fixture_copy copy{ "migration_v1" };
};

}

BOOST_FIXTURE_TEST_SUITE(PrjMigrationFixtureTests, migration_fixture)

BOOST_AUTO_TEST_CASE(TheCopy_IsAFreshFolderWithEveryFile)
{
	BOOST_TEST((copy.root() != test_util::fixture_source("migration_v1")));
	BOOST_TEST((files_of(copy.root()) == before));
	BOOST_TEST(before.contains("res/textures/plain.png"));
	BOOST_TEST(!fs::exists(copy.res() / "textures/plain.png.resource"));
}

BOOST_AUTO_TEST_CASE(TheManifest_IsOfAnOlderEngine)
{
	const prj::manifest_result loaded = prj::load_manifest(copy.root());
	BOOST_REQUIRE_MESSAGE(loaded.data.has_value(), (loaded.error ? prj::to_string(*loaded.error) : std::string{}));
	BOOST_TEST(loaded.data->format_version == 1);
	BOOST_TEST((prj::compare_engine_versions(loaded.data->engine, prj::current_engine_version()) == prj::version_match::project_older));
}

BOOST_AUTO_TEST_CASE(EveryOldDesc_MigratesToTheCurrentVersions)
{
	for (const char* file : { "worlds/w_v1.desc", "worlds/w_v2.desc", "worlds/w_v3.desc" }) {
		BOOST_TEST_INFO(file);
		BOOST_TEST(migrated(file).at("__version").as_int64() == descs->current_version("world_desc"));
	}
	for (const char* file : { "prefabs/p_v1.prefab.desc", "prefabs/p_v2.prefab.desc", "prefabs/p_v3.prefab.desc" }) {
		BOOST_TEST_INFO(file);
		BOOST_TEST(migrated(file).at("__version").as_int64() == descs->current_version("prefab_desc"));
	}
	const auto camera = migrated("cameras/cam_v1.desc");
	BOOST_TEST(camera.at("__version").as_int64() == descs->current_version("camera_desc"));
	BOOST_TEST(!camera.contains("transform"));

	const auto level = migrated("levels/old.desc");
	BOOST_TEST(level.at("__version").as_int64() == descs->current_version("level_desc"));
	BOOST_TEST(level.at("worlds").as_array()[0].at("__version").as_int64() == descs->current_version("world_desc"));
}

BOOST_AUTO_TEST_CASE(TheFixture_HoldsOlderVersionsThanTheEngine)
{
	BOOST_TEST(descs->current_version("world_desc") > 3);
	BOOST_TEST(descs->current_version("prefab_desc") > 3);
	BOOST_TEST(descs->current_version("camera_desc") > 1);
	BOOST_TEST(descs->current_version("level_desc") > 1);
}

BOOST_AUTO_TEST_CASE(ANewerFile_IsRefused)
{
	BOOST_CHECK_THROW(migrated("newer.desc"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ABrokenFile_DoesNotParse)
{
	boost::system::error_code error;
	json::parse(read_text(copy.res() / "broken.desc"), error);
	BOOST_TEST(error.failed());
}

// What S2 turns into the class unknown_type: today a type nobody registered passes as version 1.
BOOST_AUTO_TEST_CASE(AnUnknownType_PassesAsVersion1Today)
{
	BOOST_TEST(!descs->is_registered("migration_fixture_module_desc"));
	const auto obj = migrated("module/unknown.desc");
	BOOST_TEST(obj.at("__version").as_int64() == 1);
	BOOST_TEST(obj.at("hp").as_int64() == 10);
}

BOOST_AUTO_TEST_SUITE_END()
