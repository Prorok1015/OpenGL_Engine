#include <boost/test/unit_test.hpp>
#include "edt_asset_picker.h"
#include <algorithm>
#include <string>
#include <vector>

// What a reference field may point at (S56-9, #798): the filter, the list the picker offers and what it says of a text.
namespace {

namespace picker = edt::picker;

struct fake_catalog final : picker::catalog
{
	bool ready() const override { return is_ready; }
	void for_each(const std::function<void(const edt::asset_record&)>& visit) const override
	{
		for (const edt::asset_record& record : records) {
			visit(record);
		}
	}
	const edt::asset_record* find(const res::tag& path) const override
	{
		const auto found = std::ranges::find_if(records, [&](const edt::asset_record& record) { return record.path.relative() == path.relative(); });
		return found == records.end() ? nullptr : &*found;
	}
	std::size_t layer_count() const override { return layers; }

	void add(const std::string& path, std::size_t layer, const std::string& type, const char* desc_type = nullptr, bool hashed = true)
	{
		edt::asset_record record;
		record.path = res::tag::make(path);
		record.layer = layer;
		record.type = type;
		if (desc_type) {
			record.desc_type = desc_type;
		}
		if (hashed) {
			record.content_hash = "fnv1a64:0000000000000001";
		}
		records.push_back(std::move(record));
	}

	std::vector<edt::asset_record> records;
	std::size_t layers = 3;
	bool is_ready = true;
};

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

BOOST_AUTO_TEST_CASE(ReferenceValue_IsTheResPathAsAString)
{
	const fake_catalog source = sample();
	const boost::json::value value = picker::reference_value(*source.find(res::tag::make("materials/wood.desc")));
	BOOST_REQUIRE(value.is_string());
	BOOST_CHECK_EQUAL(std::string(value.as_string()), "res://materials/wood.desc");
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

BOOST_AUTO_TEST_CASE(JudgeDrop_AFileThatDoesNotFit_IsRefusedWithAReason)
{
	const fake_catalog source = sample();
	const picker::filter wanted = desc_of("scn.material");
	BOOST_CHECK(picker::judge_drop(wanted, "materials/wood.desc", &source).fits());
	BOOST_CHECK_EQUAL(picker::judge_drop(wanted, "materials/sky.desc", &source).reason, "is a desc of type 'scn.skybox', expected 'scn.material'");
	BOOST_CHECK_EQUAL(picker::judge_drop(wanted, "nowhere/a.desc", &source).reason, "is not a file of the resource tree");
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
