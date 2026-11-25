#include <boost/test/unit_test.hpp>
#include "edt_asset_filter.h"
#include "edt_asset_kind_names.h"
#include "test_temp_dir.hpp"
#include <snk/editor/edt_asset_kind_registry.h>
#include <algorithm>
#include <map>
#include <optional>
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
	const std::string text_1 = R"({ "__type": "level_desc", "__version": 3, "worlds": [] })";
	BOOST_TEST(edt::desc_type_in(text_1) == "level_desc");
	const std::string spread_out = "{\n\t\"__version\": 1,\n\t\"__type\":\"world_desc\"\n}";
	BOOST_TEST(edt::desc_type_in(spread_out) == "world_desc");
	const std::string text_2 = R"({ "name": "x" })";
	BOOST_TEST(edt::desc_type_in(text_2).empty());
	const std::string text_3 = R"({ "__type": 5 })";
	BOOST_TEST(edt::desc_type_in(text_3).empty());
	BOOST_TEST(edt::desc_type_in("").empty());
}

BOOST_AUTO_TEST_CASE(OnlyTheTopLevelTypeCounts)
{
	const std::string text_4 = R"({ "child": { "__type": "world_desc" }, "__type": "level_desc" })";
	BOOST_TEST(edt::desc_type_in(text_4) == "level_desc");
	const std::string text_5 = R"({ "list": [ { "__type": "world_desc" } ] })";
	BOOST_TEST(edt::desc_type_in(text_5).empty());
	// Kept out of the macro: MSVC's preprocessor misreads a raw string with escaped quotes in a macro argument.
	const std::string quoted = R"({ "note": "\"__type\": \"world_desc\"", "__type": "level_desc" })";
	BOOST_TEST(edt::desc_type_in(quoted) == "level_desc");
	const std::string text_6 = R"({ "a": 1, "b": { "c": [1, 2, { "__type": "x" }] }, "__type" : "prefab_desc" })";
	BOOST_TEST(edt::desc_type_in(text_6) == "prefab_desc");
	const std::string text_7 = R"({ "__type": "level_desc" })";
	BOOST_TEST(edt::desc_type_in(text_7) == "level_desc");
}

BOOST_AUTO_TEST_CASE(ABomIsSkipped_ATruncatedTextHasNoType)
{
	const std::string with_bom = std::string("\xEF\xBB\xBF") + "{ \"__type\": \"level_desc\" }";
	BOOST_TEST(edt::desc_type_in(with_bom) == "level_desc");
	const std::string text_8 = R"({ "__type": "level_de)";
	BOOST_TEST(edt::desc_type_in(text_8).empty());
	const std::string text_9 = R"({ "__typ)";
	BOOST_TEST(edt::desc_type_in(text_9).empty());
	const std::string text_10 = R"({ "__type":)";
	BOOST_TEST(edt::desc_type_in(text_10).empty());
	const std::string text_11 = R"({ "name": "unfinished)";
	BOOST_TEST(edt::desc_type_in(text_11).empty());
}

BOOST_AUTO_TEST_CASE(OnlyAPlainDescNeedsItsType)
{
	BOOST_TEST(edt::asset_kind_needs_desc_type(nullptr, "levels/a.desc"));
	BOOST_TEST(!edt::asset_kind_needs_desc_type(nullptr, "levels/a.prefab.desc"));
	BOOST_TEST(!edt::asset_kind_needs_desc_type(nullptr, "a.png"));
}

struct counting_ports
{
	edt::desc_type_cache make()
	{
		return edt::desc_type_cache({
			[this](const std::filesystem::path&, const std::filesystem::path& relative) -> std::optional<edt::desc_type_cache::stamp> {
				const auto found = stamps.find(relative.generic_string());
				if (found == stamps.end()) return std::nullopt;
				return found->second;
			},
			[this](const std::filesystem::path&, const std::filesystem::path& relative) -> std::optional<std::string> {
				++reads;
				const auto found = heads.find(relative.generic_string());
				if (found == heads.end()) return std::nullopt;
				return found->second;
			} });
	}

	std::map<std::string, edt::desc_type_cache::stamp> stamps;
	std::map<std::string, std::string> heads;
	int reads = 0;
};

BOOST_AUTO_TEST_CASE(ACachedTypeIsNotReadAgain_UntilTheFileChanges)
{
	counting_ports env;
	env.stamps["a.desc"] = { 10, 100, 1 };
	env.heads["a.desc"] = R"({ "__type": "level_desc" })";
	auto cache = env.make();
	int budget = 10;
	BOOST_TEST(cache.type_of("root", "a.desc", budget).value() == "level_desc");
	BOOST_TEST(cache.type_of("root", "a.desc", budget).value() == "level_desc");
	BOOST_TEST(cache.type_of("root", "a.desc", budget).value() == "level_desc");
	BOOST_TEST(env.reads == 1);
	BOOST_TEST(budget == 9);

	env.stamps["a.desc"] = { 11, 100, 1 };
	env.heads["a.desc"] = R"({ "__type": "world_desc" })";
	BOOST_TEST(cache.type_of("root", "a.desc", budget).value() == "world_desc");
	env.stamps["a.desc"] = { 11, 200, 1 };
	env.heads["a.desc"] = R"({ "__type": "prefab_desc" })";
	BOOST_TEST(cache.type_of("root", "a.desc", budget).value() == "prefab_desc");
	env.stamps["a.desc"] = { 11, 200, 2 };
	BOOST_TEST(cache.type_of("root", "a.desc", budget).value() == "prefab_desc");
	BOOST_TEST(env.reads == 4);
}

BOOST_AUTO_TEST_CASE(TheBudgetBoundsTheReadsOfAPass_TheRestFillsInOnTheNextOnes)
{
	counting_ports env;
	for (const char* name : { "a.desc", "b.desc", "c.desc" }) {
		env.stamps[name] = { 5, 1, 1 };
		env.heads[name] = R"({ "__type": "level_desc" })";
	}
	auto cache = env.make();
	int budget = 2;
	BOOST_TEST(cache.type_of("r", "a.desc", budget).has_value());
	BOOST_TEST(cache.type_of("r", "b.desc", budget).has_value());
	BOOST_TEST(!cache.type_of("r", "c.desc", budget).has_value());
	BOOST_TEST(env.reads == 2);
	budget = 2;
	BOOST_TEST(cache.type_of("r", "a.desc", budget).has_value());
	BOOST_TEST(cache.type_of("r", "c.desc", budget).value() == "level_desc");
	BOOST_TEST(env.reads == 3);
	BOOST_TEST(budget == 1);
}

BOOST_AUTO_TEST_CASE(AFailedRead_IsDataNotPending_AndIsNotKept)
{
	counting_ports env;
	env.stamps["a.desc"] = { 5, 1, 1 };
	auto cache = env.make();
	int budget = 5;
	const auto failed = cache.type_of("r", "a.desc", budget);
	BOOST_REQUIRE(failed.has_value());
	BOOST_TEST(failed->empty());
	BOOST_TEST(edt::asset_kind_of(nullptr, "a.desc", *failed) == "data");
	env.heads["a.desc"] = R"({ "__type": "level_desc" })";
	BOOST_TEST(cache.type_of("r", "a.desc", budget).value() == "level_desc");
	BOOST_TEST(env.reads == 2);
}

BOOST_AUTO_TEST_CASE(AFileThatIsNoRegularFile_HasNoTypeAndIsNeverRead)
{
	counting_ports env;
	auto cache = env.make();
	int budget = 5;
	BOOST_TEST(cache.type_of("r", "link.desc", budget).value().empty());
	BOOST_TEST(env.reads == 0);
	BOOST_TEST(budget == 5);
}

BOOST_AUTO_TEST_CASE(TheRealPorts_ReadAtMost4KiB_AndNotThroughALink)
{
	test_util::temp_dir dir("asset_filter_beneath");
	const std::filesystem::path root = std::filesystem::weakly_canonical(dir.path());
	dir.write("levels/a.desc", R"({ "__type": "level_desc", "__version": 3 })");
	dir.write("levels/big.desc", std::string(20000, ' ') + R"({ "__type": "level_desc" })");
	dir.write("levels/late.desc", std::string(5000, ' ') + R"({ "__type": "level_desc" })");
	edt::desc_type_cache cache;
	int budget = 10;
	BOOST_TEST(cache.type_of(root, "levels/a.desc", budget).value() == "level_desc");
	BOOST_TEST(cache.type_of(root, "levels/big.desc", budget).value().empty());
	BOOST_TEST(cache.type_of(root, "levels/late.desc", budget).value().empty());
	BOOST_TEST(cache.type_of(root, "levels/missing.desc", budget).value().empty());
	BOOST_TEST(cache.reads() == 3u);
#ifndef _WIN32
	std::error_code ec;
	std::filesystem::create_symlink(dir.path() / "levels/a.desc", dir.path() / "levels/link.desc", ec);
	if (!ec) {
		BOOST_TEST(cache.type_of(root, "levels/link.desc", budget).value().empty());
		BOOST_TEST(cache.reads() == 3u);
	}
#endif
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

// #1205: a texture desc and a picture are textures, whichever kind of the registry opens them.
BOOST_AUTO_TEST_CASE(ATextureDescAndAPicture_AreTextures_TheirKindsAreNotOfferedApart)
{
	edt::asset_kind_registry kinds;
	const auto open = [](edt::editor_context&, const res::tag&) {};
	const auto texture = kinds.add({ .id = "texture_2d_desc", .suffix = ".txm.desc", .open = open });
	const auto png = kinds.add({ .id = "picture.png", .suffix = ".png", .open = open });
	const auto pct = kinds.add({ .id = "picture.pct", .suffix = ".pct", .open = open });
	BOOST_TEST(edt::asset_kind_of(&kinds, "t/rock.txm.desc") == "texture");
	BOOST_TEST(edt::asset_kind_of(nullptr, "t/rock.txm.desc") == "texture");
	BOOST_TEST(edt::asset_kind_of(&kinds, "t/rock.png") == "texture");
	BOOST_TEST(edt::asset_kind_of(&kinds, "m/m.texture_0.pct") == "texture");
	BOOST_TEST(!edt::asset_kind_needs_desc_type(&kinds, "t/rock.txm.desc"));
	const auto offered = ids_of(edt::asset_filter_kinds(&kinds));
	BOOST_TEST(std::ranges::count(offered, "texture") == 1);
	BOOST_TEST(std::ranges::count(offered, "texture_2d_desc") == 0);
	BOOST_TEST(std::ranges::count(offered, "picture.png") == 0);
	BOOST_TEST(std::ranges::count(offered, "picture.pct") == 0);
}

BOOST_AUTO_TEST_CASE(OnlyAKindThatOpensIsOffered)
{
	edt::asset_kind_registry kinds;
	const auto opens = kinds.add({ .id = "lens", .suffix = ".lens", .open = [](edt::editor_context&, const res::tag&) {} });
	const auto drops = kinds.add({ .id = "dropper", .suffix = ".drop", .drop_into_scene = [](edt::editor_context&, const res::tag&) {} });
	const auto offered = ids_of(edt::asset_filter_kinds(&kinds));
	BOOST_TEST(std::ranges::count(offered, "lens") == 1);
	BOOST_TEST(std::ranges::count(offered, "dropper") == 0);
}

BOOST_AUTO_TEST_CASE(APickedKindThatIsNoLongerOffered_IsDropped)
{
	edt::asset_filter filter;
	filter.set_kind("lens", true);
	filter.set_kind("texture", true);
	{
		edt::asset_kind_registry kinds;
		const auto lens = kinds.add({ .id = "lens", .suffix = ".lens", .open = [](edt::editor_context&, const res::tag&) {} });
		filter.keep_only(edt::asset_filter_kinds(&kinds));
		BOOST_TEST(filter.kind_picked("lens"));
	}
	filter.keep_only(edt::asset_filter_kinds(nullptr));
	BOOST_TEST(!filter.kind_picked("lens"));
	BOOST_TEST(filter.kind_picked("texture"));
	filter.set_kind("texture", false);
	BOOST_TEST(!filter.needs_kind());
	BOOST_TEST(filter.accepts(false, "a.png", "texture"));
}

BOOST_AUTO_TEST_CASE(TickingEveryKindShowsEveryOfferedFile)
{
	edt::asset_filter every;
	for (const auto& kind : edt::asset_filter_kinds(nullptr)) every.set_kind(kind.id, true);
	const edt::asset_filter none;
	for (const char* name : { "a.png", "b.glb", "c.prefab.desc", "d.geom.desc", "e.desc", "f.frag", "g.txt", "h.level.desc", "i.world.desc", "j.shader.desc" }) {
		const std::string kind = edt::asset_kind_of(nullptr, name);
		BOOST_TEST(every.accepts(false, name, kind) == none.accepts(false, name, kind));
	}
}

BOOST_AUTO_TEST_CASE(TheNameIsCheckedBeforeAnythingElse)
{
	edt::asset_filter filter;
	filter.set_name("rock");
	BOOST_TEST(filter.accepts_name("Rock.png"));
	BOOST_TEST(!filter.accepts_name("sand.png"));
	BOOST_TEST(edt::asset_filter{}.accepts_name("anything"));
}

BOOST_AUTO_TEST_CASE(OutwardKindName_FoldsTheGlslEditorsKinds_AndLeavesTheOthers)
{
	BOOST_TEST(edt::outward_kind_name("shader_source.frag") == "shader");
	BOOST_TEST(edt::outward_kind_name("shader_source.glsl") == "shader");
	BOOST_TEST(edt::outward_kind_name("shader_desc") == "shader_desc");
	BOOST_TEST(edt::outward_kind_name("poly") == "poly");
	BOOST_TEST(edt::outward_kind_name("prefab") == "prefab");
}

BOOST_AUTO_TEST_CASE(TheGlslEditorsKinds_AreTheKindShader_AndAreNotOfferedAgain)
{
	edt::asset_kind_registry kinds;
	const auto frag = kinds.add({ .id = "shader_source.frag", .suffix = ".frag", .open = [](edt::editor_context&, const res::tag&) {} });
	const auto vert = kinds.add({ .id = "shader_source.vert", .suffix = ".vert", .open = [](edt::editor_context&, const res::tag&) {} });

	BOOST_TEST(edt::asset_kind_of(&kinds, "a/b.frag") == "shader");
	BOOST_TEST(edt::asset_kind_of(&kinds, "a/b.vert") == "shader");
	const auto offered = ids_of(edt::asset_filter_kinds(&kinds));
	BOOST_TEST(std::ranges::count(offered, std::string{ "shader" }) == 1);
	BOOST_TEST(std::ranges::count(offered, std::string{ "shader_source.frag" }) == 0);
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
