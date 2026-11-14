#include <boost/test/unit_test.hpp>
#include "edt_asset_picker.h"
#include "test_fake_asset_catalog.hpp"
#include <algorithm>
#include <string>
#include <vector>

// What a reference field may point at (S56-9, #798): the filter, the list the picker offers and what it says of a text.
namespace {

namespace picker = edt::picker;
using test_util::fake_catalog;

picker::filter desc_of(const char* type)
{
	picker::filter out;
	out.kind = desc::field_kind::desc_ref;
	out.ref_type = type;
	return out;
}

picker::filter files_of(const char* extensions)
{
	picker::filter out;
	out.kind = desc::field_kind::res_tag;
	out.extensions = extensions;
	return out;
}

fake_catalog sample()
{
	fake_catalog source;
	source.add("engine/mat.desc", 0, "desc", "scn.material");
	source.add("pkg/lit.desc", 1, "desc", "scn.material");
	source.add("materials/wood.desc", 2, "desc", "scn.material");
	source.add("materials/sky.desc", 2, "desc", "scn.skybox");
	source.add("materials/pending.desc", 2, "desc", nullptr, false);
	source.add("materials/untyped.desc", 2, "desc", nullptr, true);
	source.add("textures/Wood.PNG", 2, "texture");
	source.add("models/tree.glb", 2, "model");
	return source;
}

std::vector<std::string> paths_of(const picker::candidates_page& page)
{
	std::vector<std::string> out;
	for (const picker::candidate& item : page.items) {
		out.emplace_back(item.record->path.relative());
	}
	return out;
}

}

BOOST_AUTO_TEST_SUITE(EdtAssetPickerTests)

BOOST_AUTO_TEST_CASE(FilterOf_TakesTheDescTypeOnlyOfADescRef)
{
	desc::field_schema desc_field;
	desc_field.kind = desc::field_kind::desc_ref;
	desc_field.ref_type = "scn.material";
	BOOST_CHECK_EQUAL(picker::filter_of(desc_field).ref_type, "scn.material");

	desc::field_schema tag_field;
	tag_field.kind = desc::field_kind::res_tag;
	tag_field.ref_type = "ignored";
	tag_field.filter = "png,jpg";
	const picker::filter tag_filter = picker::filter_of(tag_field);
	BOOST_CHECK(tag_filter.ref_type.empty());
	BOOST_CHECK_EQUAL(tag_filter.extensions, "png,jpg");
}

BOOST_AUTO_TEST_CASE(ExtensionMatches_IgnoresCaseAndTheSpellingOfTheList)
{
	BOOST_CHECK(picker::extension_matches("a/b.PNG", "png"));
	BOOST_CHECK(picker::extension_matches("a/b.png", "*.jpg; .png"));
	BOOST_CHECK(picker::extension_matches("a/b.anything", ""));
	BOOST_CHECK(!picker::extension_matches("a/b.pngx", "png"));
	BOOST_CHECK(!picker::extension_matches("a/pngfile", "png"));
}

BOOST_AUTO_TEST_CASE(LayerLabel_ProjectOnTop_EngineAtTheBottom_PackagesBetween)
{
	BOOST_CHECK_EQUAL(picker::layer_label(2, 3), "project");
	BOOST_CHECK_EQUAL(picker::layer_label(1, 3), "package");
	BOOST_CHECK_EQUAL(picker::layer_label(0, 3), "engine");
	BOOST_CHECK_EQUAL(picker::layer_label(1, 2), "project");
	BOOST_CHECK_EQUAL(picker::layer_label(0, 2), "engine");
	BOOST_CHECK_EQUAL(picker::layer_label(0, 1), "project");
}

BOOST_AUTO_TEST_CASE(Judge_AskedForADescType_AcceptsThatTypeOnly_AndSaysWhy)
{
	const fake_catalog source = sample();
	const picker::filter wanted = desc_of("scn.material");
	const auto judge = [&](const char* path) { return picker::judge(wanted, *source.find(res::tag::make(path))); };

	BOOST_CHECK(judge("materials/wood.desc").fits());
	const picker::verdict other = judge("materials/sky.desc");
	BOOST_CHECK(other.outcome == picker::verdict::result::rejected);
	BOOST_CHECK_EQUAL(other.reason, "is a desc of type 'scn.skybox', expected 'scn.material'");
	BOOST_CHECK_EQUAL(judge("materials/untyped.desc").reason, "is a desc with no type, expected 'scn.material'");
	BOOST_CHECK(judge("materials/pending.desc").outcome == picker::verdict::result::unknown);
	BOOST_CHECK_EQUAL(judge("textures/Wood.PNG").reason, "is not a desc file");
}

BOOST_AUTO_TEST_CASE(Judge_ADescRefWithNoType_TakesEveryDesc_ATagWithExtensions_OnlyThose)
{
	const fake_catalog source = sample();
	picker::filter any_desc;
	any_desc.kind = desc::field_kind::desc_ref;
	BOOST_CHECK(picker::judge(any_desc, *source.find(res::tag::make("materials/pending.desc"))).fits());
	BOOST_CHECK(!picker::judge(any_desc, *source.find(res::tag::make("models/tree.glb"))).fits());

	const picker::filter textures = files_of("png,jpg");
	BOOST_CHECK(picker::judge(textures, *source.find(res::tag::make("textures/Wood.PNG"))).fits());
	BOOST_CHECK_EQUAL(picker::judge(textures, *source.find(res::tag::make("models/tree.glb"))).reason, "does not match the filter 'png,jpg'");
	BOOST_CHECK(picker::judge(files_of(""), *source.find(res::tag::make("models/tree.glb"))).fits());
}

BOOST_AUTO_TEST_CASE(Candidates_ProjectFirstThenPackagesThenEngine_OnlyWhatFits)
{
	const fake_catalog source = sample();
	const picker::candidates_page page = picker::candidates(source, desc_of("scn.material"), "", 50);
	BOOST_CHECK((paths_of(page) == std::vector<std::string>{ "materials/wood.desc", "pkg/lit.desc", "engine/mat.desc" }));
	BOOST_CHECK_EQUAL(page.total, 3u);
	BOOST_CHECK_EQUAL(page.items.front().layer_label, "project");
	BOOST_CHECK_EQUAL(page.items[1].layer_label, "package");
	BOOST_CHECK_EQUAL(page.items.back().layer_label, "engine");
	// the desc whose type is not read yet is not listed but counted
	BOOST_CHECK_EQUAL(page.unread, 1u);
}

BOOST_AUTO_TEST_CASE(Candidates_AQueryMatchesThePathWhateverTheCase_AndTheLimitCutsTheList)
{
	const fake_catalog source = sample();
	const picker::candidates_page woods = picker::candidates(source, files_of(""), "WOOD", 50);
	BOOST_CHECK((paths_of(woods) == std::vector<std::string>{ "materials/wood.desc", "textures/Wood.PNG" }));

	const picker::candidates_page cut = picker::candidates(source, files_of(""), "", 2);
	BOOST_CHECK_EQUAL(cut.items.size(), 2u);
	BOOST_CHECK_EQUAL(cut.total, 8u);
}

BOOST_AUTO_TEST_CASE(Candidates_AnEmptyCatalogOrNoMatch_IsAnEmptyPage)
{
	fake_catalog empty;
	BOOST_CHECK(picker::candidates(empty, files_of(""), "", 10).items.empty());
	const fake_catalog source = sample();
	const picker::candidates_page none = picker::candidates(source, files_of(""), "zzz", 10);
	BOOST_CHECK(none.items.empty());
	BOOST_CHECK_EQUAL(none.total, 0u);
}

BOOST_AUTO_TEST_CASE(ReferenceValue_IsTheResPathAsAString_ForAFileWithoutAnId)
{
	const fake_catalog source = sample();
	const boost::json::value value = picker::reference_value(*source.find(res::tag::make("materials/wood.desc")));
	BOOST_REQUIRE(value.is_string());
	BOOST_CHECK_EQUAL(std::string(value.as_string()), "res://materials/wood.desc");
}

// #773: a file with an ID is written as {guid, path}, so the reference survives a rename.
BOOST_AUTO_TEST_CASE(ReferenceValue_IsTheGuidAndThePath_ForAFileWithAnId)
{
	fake_catalog source = sample();
	edt::asset_record& wood = source.records[2];
	BOOST_REQUIRE(wood.path.relative() == "materials/wood.desc");
	wood.id = res::asset_id_from_text("3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f");
	const boost::json::value value = picker::reference_value(wood);
	BOOST_REQUIRE(value.is_object());
	BOOST_CHECK_EQUAL(value.as_object().size(), 2u);
	BOOST_CHECK_EQUAL(std::string(value.at("guid").as_string()), "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f");
	BOOST_CHECK_EQUAL(std::string(value.at("path").as_string()), "res://materials/wood.desc");
}

BOOST_AUTO_TEST_CASE(JudgeText_SaysNothingForWhatIsNotItsBusiness)
{
	fake_catalog source = sample();
	const picker::filter wanted = desc_of("scn.material");
	BOOST_CHECK(picker::judge_text(wanted, "", source).severity == picker::reference_state::level::ok);
	// no protocol: the field's own check speaks of that
	BOOST_CHECK(picker::judge_text(wanted, "materials/none.desc", source).severity == picker::reference_state::level::ok);
	BOOST_CHECK(picker::judge_text(wanted, "memory://x/none.desc", source).severity == picker::reference_state::level::ok);
	// the tree has not been walked: a missing file proves nothing
	source.is_ready = false;
	BOOST_CHECK(picker::judge_text(wanted, "res://materials/none.desc", source).severity == picker::reference_state::level::ok);
}

BOOST_AUTO_TEST_CASE(JudgeText_ABrokenReference_NamesTheFileAndWhatIsWrong)
{
	const fake_catalog source = sample();
	const picker::filter wanted = desc_of("scn.material");

	const picker::reference_state missing = picker::judge_text(wanted, "res://materials/none.desc", source);
	BOOST_CHECK(missing.severity == picker::reference_state::level::error);
	BOOST_CHECK_EQUAL(missing.text, "file not found: res://materials/none.desc");

	const picker::reference_state wrong = picker::judge_text(wanted, "res://materials/sky.desc", source);
	BOOST_CHECK(wrong.severity == picker::reference_state::level::error);
	BOOST_CHECK_EQUAL(wrong.text, "'res://materials/sky.desc' is a desc of type 'scn.skybox', expected 'scn.material'");

	const picker::reference_state pending = picker::judge_text(wanted, "res://materials/pending.desc", source);
	BOOST_CHECK(pending.severity == picker::reference_state::level::note);

	BOOST_CHECK(picker::judge_text(wanted, "res://materials/wood.desc", source).severity == picker::reference_state::level::ok);
	// backslashes are slashes
	BOOST_CHECK(picker::judge_text(wanted, "res://materials\\wood.desc", source).severity == picker::reference_state::level::ok);
}

BOOST_AUTO_TEST_CASE(JudgeText_AMissingFile_IsANoteWhileTheListIsBehind_AnErrorOnceItIsSettled)
{
	fake_catalog source = sample();
	const picker::filter wanted = desc_of("scn.material");
	source.is_settled = false;
	const picker::reference_state behind = picker::judge_text(wanted, "res://materials/new.desc", source);
	BOOST_CHECK(behind.severity == picker::reference_state::level::note);
	BOOST_CHECK_EQUAL(behind.text, "res://materials/new.desc is not in the asset list yet");
	source.is_settled = true;
	BOOST_CHECK(picker::judge_text(wanted, "res://materials/new.desc", source).severity == picker::reference_state::level::error);
}

BOOST_AUTO_TEST_CASE(CachedCandidates_WalkTheFilesOnlyWhenTheQueryTheFilterOrTheRevisionChanged)
{
	fake_catalog source = sample();
	picker::page_cache cache;
	const picker::filter wanted = desc_of("scn.material");
	const picker::candidates_page& first = picker::cached_candidates(cache, source, wanted, "", 50);
	BOOST_CHECK_EQUAL(first.total, 3u);
	BOOST_CHECK_EQUAL(source.walks, 1);
	picker::cached_candidates(cache, source, wanted, "", 50);
	picker::cached_candidates(cache, source, wanted, "", 50);
	BOOST_CHECK_EQUAL(source.walks, 1);

	BOOST_CHECK_EQUAL(picker::cached_candidates(cache, source, wanted, "wood", 50).total, 1u);
	BOOST_CHECK_EQUAL(source.walks, 2);
	picker::cached_candidates(cache, source, desc_of("scn.skybox"), "wood", 50);
	BOOST_CHECK_EQUAL(source.walks, 3);
	picker::cached_candidates(cache, source, desc_of("scn.skybox"), "wood", 10);
	BOOST_CHECK_EQUAL(source.walks, 4);

	// the catalog changed: the list is made again, with the new file
	source.add("materials/oak.desc", 2, "desc", "scn.material");
	++source.version;
	BOOST_CHECK_EQUAL(picker::cached_candidates(cache, source, wanted, "", 50).total, 4u);
	BOOST_CHECK_EQUAL(source.walks, 5);
}

BOOST_AUTO_TEST_CASE(JudgeDrop_AFileThatDoesNotFit_IsRefusedWithAReason)
{
	const fake_catalog source = sample();
	const picker::filter wanted = desc_of("scn.material");
	BOOST_CHECK(picker::judge_drop(wanted, "materials/wood.desc", &source).fits());
	BOOST_CHECK_EQUAL(picker::judge_drop(wanted, "materials/sky.desc", &source).reason, "is a desc of type 'scn.skybox', expected 'scn.material'");
	// a file the records do not hold yet (just imported) is not refused: it comes from the browser, so it is there
	const picker::verdict unseen = picker::judge_drop(wanted, "nowhere/a.desc", &source);
	BOOST_CHECK(unseen.outcome == picker::verdict::result::unknown);
	BOOST_CHECK_EQUAL(unseen.reason, "is not in the asset list yet");
	BOOST_CHECK(picker::judge_drop(wanted, "materials/pending.desc", &source).outcome == picker::verdict::result::unknown);
}

BOOST_AUTO_TEST_CASE(JudgeDrop_WithoutACatalogOrBeforeTheFirstWalk_OnlyTheExtensionsCount)
{
	const picker::filter textures = files_of("png");
	BOOST_CHECK(picker::judge_drop(textures, "textures/a.png", nullptr).fits());
	BOOST_CHECK_EQUAL(picker::judge_drop(textures, "models/a.glb", nullptr).reason, "does not match the filter 'png'");
	fake_catalog source = sample();
	source.is_ready = false;
	BOOST_CHECK(picker::judge_drop(textures, "textures/anything.png", &source).fits());
	BOOST_CHECK(!picker::judge_drop(textures, "models/a.glb", &source).fits());
}

BOOST_AUTO_TEST_SUITE_END()
