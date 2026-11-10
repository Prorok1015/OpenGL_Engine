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

// An agent chooses the asset name. Replacing the model's stem anywhere in a path let "raw.obj" imported as "obj" turn a
// resource "x.raw" into "x.obj", which the importer's adapter would then parse inside the editor.
BOOST_AUTO_TEST_CASE(BuildRemap_ReplacesTheStemOnlyAtTheStartOfTheFileName_SoNoExtensionChanges)
{
	edt::asset_exporter exporter(*rs);
	edt::import_result raw;
	raw.source_path = "models/raw.obj";
	raw.root_tag = res::tag{ res::tag::memory, "raw/raw.prefab.desc" };
	const res::tag embedded{ res::tag::memory, "raw/__embedded_txm/x.raw" };
	raw.all_tags = { raw.root_tag, embedded };
	const auto remap = exporter.build_remap(raw, "obj", "assets/obj");
	BOOST_TEST(remap.at(raw.root_tag).string() == "res://assets/obj/obj.prefab.desc");
	BOOST_TEST(remap.at(embedded).string() == "res://assets/obj/__embedded_txm/x.raw");
	BOOST_TEST(edt::asset_exporter::remap_problem(remap, "obj", "assets/obj").empty());

	edt::import_result tga;
	tga.source_path = "models/tga.obj";
	tga.root_tag = res::tag{ res::tag::memory, "tga/tga.prefab.desc" };
	const res::tag texture{ res::tag::memory, "tga/textures/x.tga" };
	tga.all_tags = { tga.root_tag, texture };
	const auto tga_remap = exporter.build_remap(tga, "fbx", "assets/fbx");
	BOOST_TEST(tga_remap.at(texture).string() == "res://assets/fbx/textures/x.tga");
}

BOOST_AUTO_TEST_CASE(RemapProblem_RefusesANameAnImportDoesNotGive_AndAChangedExtension)
{
	const auto tag = [](const char* text) { return res::tag::make(text); };
	std::unordered_map<res::tag, res::tag> good;
	good[res::tag{ res::tag::memory, "m/m.prefab.desc" }] = tag("assets/x/x.prefab.desc");
	good[res::tag{ res::tag::memory, "m/m.geom.desc" }] = tag("assets/x/x.geom.desc");
	good[res::tag{ res::tag::memory, "m/textures/a.png" }] = tag("assets/x/textures/a.png");
	BOOST_TEST(edt::asset_exporter::remap_problem(good, "x", "assets/x").empty());

	std::unordered_map<res::tag, res::tag> model_file;
	model_file[res::tag{ res::tag::memory, "m/textures/a.png" }] = tag("assets/x/textures/a.obj");
	BOOST_TEST(!edt::asset_exporter::remap_problem(model_file, "x", "assets/x").empty());

	// A name an import does give, but not to this resource: the extension changed.
	std::unordered_map<res::tag, res::tag> changed;
	changed[res::tag{ res::tag::memory, "m/__embedded_txm/x.raw" }] = tag("assets/x/__embedded_txm/x.png");
	BOOST_TEST(!edt::asset_exporter::remap_problem(changed, "x", "assets/x").empty());

	std::unordered_map<res::tag, res::tag> outside;
	outside[res::tag{ res::tag::memory, "m/m.prefab.desc" }] = tag("assets/elsewhere/x.prefab.desc");
	BOOST_TEST(!edt::asset_exporter::remap_problem(outside, "x", "assets/x").empty());
}

BOOST_AUTO_TEST_SUITE_END()
