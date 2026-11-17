#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "edt_asset_exporter.h"
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <unordered_map>

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

// What the export dialog lets a person type or browse to (#741): a plain path below res://, never a way out of it.
// The rules are check_agent_folder's (#741 review): one set for a person and an agent.
BOOST_AUTO_TEST_CASE(FolderProblem_RefusesEverythingButAPlainRelativeFolder)
{
	BOOST_TEST(edt::asset_exporter::folder_problem("assets/x").empty());
	BOOST_TEST(edt::asset_exporter::folder_problem("x").empty());
	BOOST_TEST(edt::asset_exporter::folder_problem("").empty());
	for (const std::string_view folder : { "..", ".", "../x", "a/../b", "a/./b", "a/..", "/abs/x", "C:/x", "C:x", "a\\b",
			"res://a", "a://b", "a//b", "a/", "//server/share", "NUL", "a/CON", "assets/com1.txt", "tree.", "a /b", "a<b", "a?b",
			"a|b", "a*b" }) {
		BOOST_TEST_INFO("folder: " << folder);
		BOOST_TEST(!edt::asset_exporter::folder_problem(folder).empty());
	}
	BOOST_TEST(!edt::asset_exporter::folder_problem(std::string_view{ "a\0b", 3 }).empty());
	BOOST_TEST(!edt::asset_exporter::folder_problem("a\nb").empty());
	BOOST_TEST(!edt::asset_exporter::folder_problem(std::string(600, 'a')).empty());
}

BOOST_AUTO_TEST_CASE(AssetNameProblem_RefusesAPathInTheName)
{
	BOOST_TEST(edt::asset_exporter::asset_name_problem("tree").empty());
	BOOST_TEST(edt::asset_exporter::asset_name_problem("tree v2").empty());
	for (const std::string_view name : { "", ".", "..", "../x", "a/b", "a\\b", "C:x", "a\tb", "a://b", "res://x", "NUL", "con.txt",
			"tree.", "tree ", "a?b" }) {
		BOOST_TEST_INFO("name: " << name);
		BOOST_TEST(!edt::asset_exporter::asset_name_problem(name).empty());
	}
}

// The person's check keeps any name the importer gives (a .dds texture too); only the agent's adds the naming rules.
BOOST_AUTO_TEST_CASE(DestinationProblem_HoldsTheFolder_RemapProblemAlsoTheNames)
{
	edt::asset_exporter exporter(*rs);
	edt::import_result imported;
	imported.source_path = "models/tri.obj";
	imported.root_tag = res::tag{ res::tag::memory, "tri/tri.prefab.desc" };
	const res::tag dds{ res::tag::memory, "tri/textures/bark.dds" };
	imported.all_tags = { imported.root_tag, dds };
	const auto remap = exporter.build_remap(imported, "tree", "assets/tree");
	BOOST_TEST(remap.at(dds).string() == "res://assets/tree/textures/bark.dds");
	BOOST_TEST(edt::asset_exporter::destination_problem(remap, "tree", "assets/tree").empty());
	BOOST_TEST(!edt::asset_exporter::remap_problem(remap, "tree", "assets/tree").empty());
	std::unordered_map<res::tag, res::tag> prefab_only{ { imported.root_tag, remap.at(imported.root_tag) } };
	BOOST_TEST(edt::asset_exporter::remap_problem(prefab_only, "tree", "assets/tree").empty());

	const auto escaping = exporter.build_remap(imported, "tree", "../tree");
	BOOST_TEST(!edt::asset_exporter::destination_problem(escaping, "tree", "../tree").empty());
	BOOST_TEST(!edt::asset_exporter::remap_problem(escaping, "tree", "../tree").empty());

	const auto absolute = exporter.build_remap(imported, "tree", "/tmp/tree");
	BOOST_TEST(!edt::asset_exporter::destination_problem(absolute, "tree", "/tmp/tree").empty());

	std::unordered_map<res::tag, res::tag> other_protocol;
	other_protocol[imported.root_tag] = res::tag{ res::tag::memory, "assets/tree/tree.prefab.desc" };
	BOOST_TEST(!edt::asset_exporter::destination_problem(other_protocol, "tree", "assets/tree").empty());

	std::unordered_map<res::tag, res::tag> dot_dot;
	dot_dot[imported.root_tag] = res::tag::make("assets/tree/../tree.prefab.desc");
	BOOST_TEST(!edt::asset_exporter::destination_problem(dot_dot, "tree", "assets/tree").empty());

	std::unordered_map<res::tag, res::tag> reserved;
	reserved[dds] = res::tag::make("assets/tree/textures/nul.dds");
	BOOST_TEST(!edt::asset_exporter::destination_problem(reserved, "tree", "assets/tree").empty());
}

// An empty folder is the root of res://, as a Browse to the resources folder gives it.
BOOST_AUTO_TEST_CASE(TheRootOfRes_IsAFolderToo)
{
	edt::asset_exporter exporter(*rs);
	const auto imported = make_import("models/tri.obj");
	const auto remap = exporter.build_remap(imported, "box", "");
	BOOST_TEST(remap.at(imported.root_tag).string() == "res://box.prefab.desc");
	BOOST_TEST(edt::asset_exporter::destination_problem(remap, "box", "").empty());
}

// A write the resource system refuses ends the export with false and says which file; nothing lands outside res://.
BOOST_AUTO_TEST_CASE(ExportToProject_ARefusedWrite_ReturnsFalse_AndNamesTheFile)
{
	const std::string outside = std::format("snk_export_741_{}", std::chrono::steady_clock::now().time_since_epoch().count());
	const res::tag source{ res::tag::memory, "tri/tri.prefab.desc" };
	BOOST_REQUIRE(rs->store_sync(source, test_util::to_bytes("{}")));
	edt::import_result imported;
	imported.source_path = "models/tri.obj";
	imported.root_tag = source;
	imported.all_tags.push_back(source);
	std::unordered_map<res::tag, res::tag> remap;
	remap[source] = res::tag::make("../" + outside + "/tri.prefab.desc");

	edt::asset_exporter exporter(*rs);
	edt::export_progress progress;
	std::vector<res::tag> written;
	BOOST_TEST(!exporter.export_to_project(imported, remap, &progress, nullptr, &written, true));
	BOOST_TEST(!progress.success.load());
	BOOST_TEST(progress.finished.load());
	BOOST_TEST(progress.error.find("tri.prefab.desc") != std::string::npos);
	BOOST_TEST(written.empty());
	BOOST_TEST(!std::filesystem::exists(outside));
}

// A resource that cannot be read or a desc that is not JSON is no longer skipped or written raw: the export says so.
BOOST_AUTO_TEST_CASE(ExportToProject_AnUnreadableOrBrokenResource_StopsTheExport)
{
	const std::string folder = std::format("tests/export_741_{}", std::chrono::steady_clock::now().time_since_epoch().count());
	edt::asset_exporter exporter(*rs);

	edt::import_result missing;
	missing.source_path = "models/gone.obj";
	missing.root_tag = res::tag{ res::tag::memory, "gone/gone.prefab.desc" };
	missing.all_tags.push_back(missing.root_tag);
	edt::export_progress missing_progress;
	std::vector<res::tag> missing_written;
	BOOST_TEST(!exporter.export_to_project(missing, exporter.build_remap(missing, "gone", folder), &missing_progress, nullptr,
		&missing_written, true));
	BOOST_TEST(missing_progress.error.find("gone.prefab.desc") != std::string::npos);
	BOOST_TEST(missing_written.empty());

	edt::import_result broken;
	broken.source_path = "models/bad.obj";
	broken.root_tag = res::tag{ res::tag::memory, "bad/bad.prefab.desc" };
	const res::tag texture{ res::tag::memory, "bad/textures/a.png" };
	BOOST_REQUIRE(rs->store_sync(texture, test_util::to_bytes("png")));
	BOOST_REQUIRE(rs->store_sync(broken.root_tag, test_util::to_bytes("{ not json")));
	broken.all_tags = { texture, broken.root_tag };
	edt::export_progress broken_progress;
	std::vector<res::tag> broken_written;
	BOOST_TEST(!exporter.export_to_project(broken, exporter.build_remap(broken, "bad", folder), &broken_progress, nullptr,
		&broken_written, true));
	BOOST_TEST(broken_progress.error.find("bad.prefab.desc") != std::string::npos);
	BOOST_TEST(!broken_progress.success.load());
	BOOST_TEST(broken_written.size() == 1u);
	BOOST_TEST(!std::filesystem::exists(std::filesystem::path{ "res" } / folder / "bad.prefab.desc"));

	std::error_code ignored;
	std::filesystem::remove_all(std::filesystem::path{ "res" } / folder, ignored);
}

// The importer lists an external texture it did not find: the rest is exported and the missing one named.
BOOST_AUTO_TEST_CASE(ExportToProject_ALeafTheImportNeverStored_IsLeftOutAndNamed)
{
	const std::string folder = std::format("tests/export_741_skip_{}", std::chrono::steady_clock::now().time_since_epoch().count());
	edt::asset_exporter exporter(*rs);
	edt::import_result imported;
	imported.source_path = "models/leaf.obj";
	imported.root_tag = res::tag{ res::tag::memory, "leaf/leaf.prefab.desc" };
	const res::tag absent{ res::tag::memory, "leaf/textures/absent.png" };
	BOOST_REQUIRE(rs->store_sync(imported.root_tag, test_util::to_bytes("{}")));
	imported.all_tags = { absent, imported.root_tag };
	const auto remap = exporter.build_remap(imported, "leaf", folder);

	edt::export_progress progress;
	std::vector<res::tag> written;
	BOOST_TEST(exporter.export_to_project(imported, remap, &progress, nullptr, &written, true));
	BOOST_TEST(progress.success.load());
	BOOST_REQUIRE(progress.skipped.size() == 1u);
	BOOST_TEST(progress.skipped.front().string() == remap.at(absent).string());
	BOOST_TEST(written.size() == 1u);

	std::error_code ignored;
	std::filesystem::remove_all(std::filesystem::path{ "res" } / folder, ignored);
}

BOOST_AUTO_TEST_SUITE_END()
