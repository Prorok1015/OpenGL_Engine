#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_asset_exporter.h"
#include <string>

namespace {

edt::import_result make_import(const std::filesystem::path& source)
{
	edt::import_result imported;
	imported.source_path = source;
	imported.root_tag = res::tag{ res::tag::memory, "tri/tri.prefab.desc" };
	imported.all_tags.push_back(imported.root_tag);
	imported.all_tags.push_back(res::tag{ res::tag::memory, "tri/tri_mesh.geom" });
	return imported;
}

}

BOOST_FIXTURE_TEST_SUITE(EdtAssetExporterTests, test_util::scene_env)

BOOST_AUTO_TEST_CASE(BuildRemap_NamedDifferentlyFromStem_ReplacesTheStem)
{
	edt::asset_exporter exporter(*rs);
	const auto imported = make_import("models/tri.obj");
	const auto remap = exporter.build_remap(imported, "box", "assets/box");

	BOOST_TEST(remap.size() == 2u);
	BOOST_TEST(remap.at(imported.root_tag).string() == "res://assets/box/box.prefab.desc");
}

// An empty stem used to make replace_stem search for "" (matches at every position) and grow the string until bad_alloc.
BOOST_AUTO_TEST_CASE(BuildRemap_EmptySourcePath_TerminatesAndKeepsNames)
{
	edt::asset_exporter exporter(*rs);
	const auto imported = make_import({});
	const auto remap = exporter.build_remap(imported, "box", "assets/box");

	BOOST_TEST(remap.size() == 2u);
	BOOST_TEST(remap.at(imported.root_tag).string() == "res://assets/box/tri/tri.prefab.desc");
}

BOOST_AUTO_TEST_CASE(BuildRemap_SourcePathWithTrailingSeparator_TerminatesAndKeepsNames)
{
	edt::asset_exporter exporter(*rs);
	const auto imported = make_import("models/");
	const auto remap = exporter.build_remap(imported, "box", "assets/box");

	BOOST_TEST(remap.size() == 2u);
	BOOST_TEST(remap.at(imported.root_tag).string() == "res://assets/box/tri/tri.prefab.desc");
}

BOOST_AUTO_TEST_CASE(BuildRemap_ExtensionOnlySourceName_Terminates)
{
	edt::asset_exporter exporter(*rs);
	const auto imported = make_import(".fbx");
	const auto remap = exporter.build_remap(imported, "box", "assets/box");

	BOOST_TEST(remap.size() == 2u);
	BOOST_TEST(remap.at(imported.root_tag).string() == "res://assets/box/tri/tri.prefab.desc");
}

BOOST_AUTO_TEST_SUITE_END()
