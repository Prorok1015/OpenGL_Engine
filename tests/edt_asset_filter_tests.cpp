#include <boost/test/unit_test.hpp>
#include "edt_asset_filter.h"
#include "test_temp_dir.hpp"
#include <snk/editor/edt_asset_kind_registry.h>
#include <algorithm>
#include <string>
#include <vector>

// #1191: the asset browser's filter by kind: one kind per file, folders always pass, the kinds combine with the name.

namespace {

std::vector<std::string> ids_of(const std::vector<edt::asset_filter_kind>& kinds)
{
	std::vector<std::string> ids;
	for (const auto& kind : kinds) ids.push_back(kind.id);
	return ids;
}

}

BOOST_AUTO_TEST_SUITE(EdtAssetFilter)

BOOST_AUTO_TEST_CASE(AFileHasOneKind_ByItsSuffix)
{
	BOOST_TEST(edt::asset_kind_of(nullptr, "a/b.prefab.desc") == "prefab");
	BOOST_TEST(edt::asset_kind_of(nullptr, "a/b.geom.desc") == "mesh");
	BOOST_TEST(edt::asset_kind_of(nullptr, "a/b.poly.desc") == "mesh");
	BOOST_TEST(edt::asset_kind_of(nullptr, "a/b.world.desc") == "world");
	BOOST_TEST(edt::asset_kind_of(nullptr, "a/b.level.desc") == "level");
	BOOST_TEST(edt::asset_kind_of(nullptr, "a/b.shader.desc") == "shader_desc");
	BOOST_TEST(edt::asset_kind_of(nullptr, "a/b.desc") == "data");
	BOOST_TEST(edt::asset_kind_of(nullptr, "a/b.png") == "texture");
	BOOST_TEST(edt::asset_kind_of(nullptr, "a/b.glb") == "model");
	BOOST_TEST(edt::asset_kind_of(nullptr, "a/b.frag") == "shader");
	BOOST_TEST(edt::asset_kind_of(nullptr, "a/b.txt") == "file");
}

BOOST_AUTO_TEST_CASE(TheSuffixIsReadWithoutCase)
{
	BOOST_TEST(edt::asset_kind_of(nullptr, "A/B.PREFAB.DESC") == "prefab");
	BOOST_TEST(edt::asset_kind_of(nullptr, "A/B.Geom.Desc") == "mesh");
}

BOOST_AUTO_TEST_CASE(ALevelWithoutALevelSuffix_IsKnownByTheTypeOfItsDesc)
{
	BOOST_TEST(edt::asset_kind_of(nullptr, "levels/editor_level.desc", "level_desc") == "level");
	BOOST_TEST(edt::asset_kind_of(nullptr, "levels/w.desc", "world_desc") == "world");
	BOOST_TEST(edt::asset_kind_of(nullptr, "levels/p.desc", "prefab_desc") == "prefab");
	BOOST_TEST(edt::asset_kind_of(nullptr, "levels/m.desc", "material_desc") == "data");
	BOOST_TEST(edt::asset_kind_of(nullptr, "levels/m.desc") == "data");
}

BOOST_AUTO_TEST_CASE(TheTypeOfADescIsReadFromItsFirstBytes)
{
	test_util::temp_dir dir("asset_filter_sniff");
	BOOST_TEST(edt::sniff_desc_type(dir.write("a.desc", R"({ "__type": "level_desc", "__version": 3, "worlds": [] })")) == "level_desc");
	BOOST_TEST(edt::sniff_desc_type(dir.write("b.desc", "{\n\t\"__version\": 1,\n\t\"__type\":\"world_desc\"\n}")) == "world_desc");
	BOOST_TEST(edt::sniff_desc_type(dir.write("c.desc", R"({ "name": "x" })")).empty());
	BOOST_TEST(edt::sniff_desc_type(dir.write("d.desc", R"({ "__type": 5 })")).empty());
	BOOST_TEST(edt::sniff_desc_type(dir.path() / "missing.desc").empty());
}

BOOST_AUTO_TEST_CASE(AKindOfAModule_IsTheKindOfItsFiles_ThePolyKindIsTheMeshKind)
{
	edt::asset_kind_registry kinds;
	const auto module_kind = kinds.add({ .id = "lens", .suffix = ".lens", .open = [](edt::editor_context&, const res::tag&) {} });
	const auto poly = kinds.add({ .id = "poly", .suffix = ".poly.desc", .open = [](edt::editor_context&, const res::tag&) {} });
	BOOST_TEST(edt::asset_kind_of(&kinds, "x.lens") == "lens");
	BOOST_TEST(edt::asset_kind_of(&kinds, "x.poly.desc") == "mesh");
	const auto offered = ids_of(edt::asset_filter_kinds(&kinds));
	BOOST_TEST(std::ranges::count(offered, "lens") == 1);
	BOOST_TEST(std::ranges::count(offered, "poly") == 0);
	BOOST_TEST(std::ranges::count(offered, "mesh") == 1);
}

BOOST_AUTO_TEST_CASE(TheFixedKindsAreOfferedOnceEach)
{
	const auto offered = ids_of(edt::asset_filter_kinds(nullptr));
	for (const std::string id : { "prefab", "level", "world", "mesh", "data", "shader_desc", "texture", "model", "shader", "file" }) {
		BOOST_TEST(std::ranges::count(offered, id) == 1);
	}
}

BOOST_AUTO_TEST_CASE(NoKindPicked_PassesEveryFile)
{
	edt::asset_filter filter;
	BOOST_TEST(!filter.active());
	BOOST_TEST(filter.accepts(false, "a.png", ""));
	BOOST_TEST(filter.accepts(false, "a.prefab.desc", ""));
}

BOOST_AUTO_TEST_CASE(OneOrSeveralKinds_FilterTheFiles_FoldersStayVisible)
{
	edt::asset_filter filter;
	filter.set_kind("texture", true);
	BOOST_TEST(filter.accepts(false, "a.png", "texture"));
	BOOST_TEST(!filter.accepts(false, "a.glb", "model"));
	BOOST_TEST(filter.accepts(true, "folder", ""));
	filter.set_kind("model", true);
	BOOST_TEST(filter.accepts(false, "a.glb", "model"));
	BOOST_TEST(filter.accepts(false, "a.png", "texture"));
	BOOST_TEST(!filter.accepts(false, "a.prefab.desc", "prefab"));
	filter.set_kind("texture", false);
	BOOST_TEST(!filter.accepts(false, "a.png", "texture"));
	filter.clear_kinds();
	BOOST_TEST(filter.accepts(false, "a.prefab.desc", "prefab"));
}

BOOST_AUTO_TEST_CASE(TheKindsCombineWithTheNameAsBoth_AndTheNameIsReadWithoutCase)
{
	edt::asset_filter filter;
	filter.set_name("RoCk");
	filter.set_kind("texture", true);
	BOOST_TEST(filter.accepts(false, "Rock_diffuse.png", "texture"));
	BOOST_TEST(!filter.accepts(false, "Rock.glb", "model"));
	BOOST_TEST(!filter.accepts(false, "sand.png", "texture"));
	BOOST_TEST(filter.accepts(true, "sand", ""));
	BOOST_TEST(filter.active());
}

BOOST_AUTO_TEST_CASE(TheSummaryNamesTheChoice)
{
	edt::asset_filter filter;
	BOOST_TEST(filter.summary() == "All");
	filter.set_kind("texture", true);
	BOOST_TEST(filter.summary() == "Texture");
	filter.set_kind("model", true);
	BOOST_TEST(filter.summary() == "2 kinds");
}

BOOST_AUTO_TEST_SUITE_END()
