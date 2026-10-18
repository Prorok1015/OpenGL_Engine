#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_asset_layers.h"

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

BOOST_AUTO_TEST_SUITE_END()
