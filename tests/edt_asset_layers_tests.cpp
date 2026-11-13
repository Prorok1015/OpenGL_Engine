#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_asset_layers.h"
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// The merged res:// directory view of the asset browser (no ImGui).

namespace {

std::vector<std::string> names(const std::vector<edt::layer_entry>& entries)
{
	std::vector<std::string> out;
	for (const auto& e : entries) out.push_back(e.name);
	return out;
}

const edt::layer_entry* find(const std::vector<edt::layer_entry>& entries, const std::string& name)
{
	for (const auto& e : entries) if (e.name == name) return &e;
	return nullptr;
}

}

BOOST_AUTO_TEST_SUITE(EdtAssetLayersTests)

BOOST_AUTO_TEST_CASE(MergesEntriesOfAllLayers)
{
	test_util::temp_dir base("asset_layers_base_merge");
	test_util::temp_dir top("asset_layers_top_merge");
	base.write("engine_only.txt", "b");
	top.write("project_only.txt", "t");

	const auto entries = edt::list_merged_directory({ base.path(), top.path() }, {});

	BOOST_TEST((names(entries) == std::vector<std::string>{ "engine_only.txt", "project_only.txt" }));
	BOOST_REQUIRE(find(entries, "engine_only.txt"));
	BOOST_TEST(find(entries, "engine_only.txt")->layer == 0u);
	BOOST_TEST(find(entries, "project_only.txt")->layer == 1u);
}

BOOST_AUTO_TEST_CASE(UpperLayerShadowsTheSameRelativePath)
{
	test_util::temp_dir base("asset_layers_base_shadow");
	test_util::temp_dir top("asset_layers_top_shadow");
	base.write("shared.txt", "base");
	const auto top_file = top.write("shared.txt", "top");

	const auto entries = edt::list_merged_directory({ base.path(), top.path() }, {});

	BOOST_REQUIRE(entries.size() == 1u);
	BOOST_TEST(entries[0].layer == 1u);
	BOOST_TEST(entries[0].path == top_file);
}

BOOST_AUTO_TEST_CASE(DirectoriesMergeAndListFirst)
{
	test_util::temp_dir base("asset_layers_base_dirs");
	test_util::temp_dir top("asset_layers_top_dirs");
	base.write("shaders/a.vert", "a");
	top.write("shaders/b.frag", "b");
	top.write("aaa.txt", "x");

	const auto root = edt::list_merged_directory({ base.path(), top.path() }, {});
	BOOST_TEST((names(root) == std::vector<std::string>{ "shaders", "aaa.txt" })); // dirs before files
	BOOST_TEST(root[0].is_dir);

	const auto sub = edt::list_merged_directory({ base.path(), top.path() }, "shaders");
	BOOST_TEST((names(sub) == std::vector<std::string>{ "a.vert", "b.frag" }));
	BOOST_TEST(find(sub, "a.vert")->layer == 0u);
	BOOST_TEST(find(sub, "b.frag")->layer == 1u);
	BOOST_TEST(find(sub, "a.vert")->relative == std::filesystem::path("shaders") / "a.vert");
}

BOOST_AUTO_TEST_CASE(MissingDirectoriesAreSkipped)
{
	test_util::temp_dir base("asset_layers_base_missing");
	test_util::temp_dir top("asset_layers_top_missing");
	base.write("only_base/x.txt", "x");

	BOOST_TEST(edt::list_merged_directory({ base.path(), top.path() }, "only_base").size() == 1u);
	BOOST_TEST(edt::list_merged_directory({ base.path(), top.path() }, "nowhere").empty());
	BOOST_TEST(edt::list_merged_directory({}, {}).empty());
}

BOOST_AUTO_TEST_CASE(MergeKeyFoldsCaseOnlyWhenAsked)
{
	BOOST_TEST(edt::layer_merge_key("Foo.TXT", true) == "foo.txt");
	BOOST_TEST(edt::layer_merge_key("Foo.TXT", false) == "Foo.TXT");
}

BOOST_AUTO_TEST_CASE(DirsOnlySkipsFiles)
{
	test_util::temp_dir base("asset_layers_base_dirsonly");
	base.write("sub/x.txt", "x");
	base.write("file.txt", "f");

	const auto entries = edt::list_merged_directory({ base.path() }, {}, true);

	BOOST_TEST((names(entries) == std::vector<std::string>{ "sub" }));
}

// #850: sidecars and temporary/system files are never listed as assets, whoever calls the helper.
BOOST_AUTO_TEST_CASE(SidecarsAndIgnoredFilesAreNotListed)
{
	test_util::temp_dir base("asset_layers_base_hidden");
	test_util::temp_dir top("asset_layers_top_hidden");
	base.write("a.png", "a");
	base.write("a.png.resource", "{}");
	top.write("b.desc", "{}");
	top.write("b.desc.RESOURCE", "{}");
	top.write(".gitkeep", "");
	top.write("Thumbs.db", "");
	top.write("x.tmp", "");
	top.write("y.tmp.png", "y");

	const auto entries = edt::list_merged_directory({ base.path(), top.path() }, {});

	BOOST_TEST((names(entries) == std::vector<std::string>{ "a.png", "b.desc", "y.tmp.png" }));
}

// #854: the browser lists what the asset walk enters: no hidden folder, no link to a folder.
BOOST_AUTO_TEST_CASE(HiddenAndLinkedFoldersAreNotListed)
{
	test_util::temp_dir top("asset_layers_hidden_dirs");
	top.write(".git/config", "x");
	top.write(".idea/workspace.xml", "x");
	top.write("shown/a.png", "a");
	std::error_code ec;
	std::filesystem::create_directory_symlink(top.path() / "shown", top.path() / "linked", ec);

	BOOST_TEST((names(edt::list_merged_directory({ top.path() }, {})) == std::vector<std::string>{ "shown" }));
	BOOST_TEST((names(edt::list_merged_directory({ top.path() }, {}, true)) == std::vector<std::string>{ "shown" }));
}

// #854: in the layer that is the project folder (it holds project.snkproj) its code/, build/ and project.snkproj are no
// assets and shadow nothing below; deeper down the same names are ordinary folders.
BOOST_AUTO_TEST_CASE(TheProjectFolderLayer_HidesItsCodeBuildAndProjectFile)
{
	test_util::temp_dir base("asset_layers_base_project");
	test_util::temp_dir project("asset_layers_project_root");
	base.write("code/lib.txt", "x");
	project.write("project.snkproj", "{}");
	project.write("code/main.cpp", "x");
	project.write("build/out.bin", "x");
	project.write("textures/code/a.png", "x");
	project.write("textures/build/b.png", "x");

	const auto root = edt::list_merged_directory({ base.path(), project.path() }, {});
	BOOST_TEST((names(root) == std::vector<std::string>{ "code", "textures" }));
	BOOST_REQUIRE(find(root, "code") != nullptr);
	BOOST_TEST(find(root, "code")->layer == 0u);
	// Entered from the base layer, code/ still shows nothing of the project's code.
	BOOST_TEST((names(edt::list_merged_directory({ base.path(), project.path() }, "code")) == std::vector<std::string>{ "lib.txt" }));
	BOOST_TEST((names(edt::list_merged_directory({ base.path(), project.path() }, "textures")) == std::vector<std::string>{ "build", "code" }));
}

// #854: a layer without project.snkproj is no project folder: its code/ and build/ are listed as any folder.
BOOST_AUTO_TEST_CASE(ALayerWithoutAProjectFile_ListsCodeAndBuildLikeAnyFolder)
{
	test_util::temp_dir top("asset_layers_no_project");
	top.write("code/main.cpp", "x");
	top.write("build/out.bin", "x");

	BOOST_TEST((names(edt::list_merged_directory({ top.path() }, {})) == std::vector<std::string>{ "build", "code" }));
}

BOOST_AUTO_TEST_SUITE_END()
