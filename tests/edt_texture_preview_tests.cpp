#include <boost/test/unit_test.hpp>
#include "edt_preview_fixture.hpp"
#include "edt_data_document.h"
#include "edt_texture_preview_content.h"
#include "edt_texture_window_model.h"
#include <boost/json.hpp>
#include <optional>
#include <string>
#include <string_view>

// The texture preview content (#1205): the memory copies of the texture and of the material the plane draws with, written
// again when the source moves and given up on release. No stage and no pixels.

namespace {

namespace json = boost::json;

constexpr const char* k_texture = R"json({"__type":"texture_2d_desc","__version":1,"name":"t","header":{"wrap":"repeat"}})json";

struct texture_preview_fixture : test_util::preview_env
{
	texture_preview_fixture()
		: preview_env("edt_texture_preview")
	{
	}

	~texture_preview_fixture()
	{
		content.reset();
		document.reset();
	}

	edt::texture_preview_content& open_document(std::string_view text = k_texture)
	{
		document.emplace(services, layout());
		const edt::open_result opened = document->open(put("t.txm.desc", text));
		BOOST_REQUIRE_MESSAGE(opened.opened, opened.error);
		content.emplace(edt::texture_preview_source{
			.body = [this] { return json::value(document->own()); },
			.generation = [this] { return document->generation(); } }, services);
		return *content;
	}

	std::optional<json::object> stored(const res::tag& tag) const
	{
		const auto found = ram->files.find(tag);
		if (found == ram->files.end()) return std::nullopt;
		return json::parse(std::string_view(reinterpret_cast<const char*>(found->second.data()), found->second.size())).as_object();
	}

	std::optional<edt::data_document> document;
	std::optional<edt::texture_preview_content> content;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtTexturePreviewTests, texture_preview_fixture)

BOOST_AUTO_TEST_CASE(TheCopies_AreWritten_TheMaterialSamplesTheTextureCopy)
{
	edt::texture_preview_content& shown = open_document();
	BOOST_TEST((shown.update() == edt::preview_content::state::changed));

	const std::optional<json::object> texture = stored(shown.texture_tag());
	BOOST_REQUIRE(texture.has_value());
	BOOST_TEST(texture->at("__type").as_string() == "texture_2d_desc");
	BOOST_TEST(!texture->contains("__parent"));
	BOOST_TEST(texture->at("header").as_object().at("wrap").as_string() == "repeat");

	const std::optional<json::object> material = stored(shown.material_tag());
	BOOST_REQUIRE(material.has_value());
	BOOST_TEST(material->at("__type").as_string() == "material_desc");
	BOOST_TEST(material->at("__parent").as_string() == "res://base_material.desc");
	BOOST_TEST((material->at("defines") == json::value(json::array{ "USE_TXM_AS_DIFFUSE" })));
	const json::array& samplers = material->at("samplers").as_array();
	BOOST_REQUIRE(samplers.size() == 1u);
	BOOST_TEST(std::string_view(samplers.at(0).as_string()) == shown.texture_tag().view());
	BOOST_TEST(shown.texture_tag().view().ends_with(".txm.desc"));
	BOOST_TEST(shown.material_tag().view().ends_with(".mat.desc"));

	BOOST_TEST((shown.update() == edt::preview_content::state::unchanged));
	BOOST_TEST((shown.framing().pitch_degrees == -90.f));
}

BOOST_AUTO_TEST_CASE(AnEdit_RewritesTheTextureCopy_TheFileStays)
{
	edt::texture_preview_content& shown = open_document();
	BOOST_REQUIRE((shown.update() == edt::preview_content::state::changed));
	edt::texture_window_model model(*document);
	BOOST_REQUIRE(model.set_wrap("clamp_to_border"));

	BOOST_TEST((shown.update() == edt::preview_content::state::changed));
	const std::optional<json::object> texture = stored(shown.texture_tag());
	BOOST_REQUIRE(texture.has_value());
	BOOST_TEST(texture->at("header").as_object().at("wrap").as_string() == "clamp_to_border");
	BOOST_TEST(document->is_dirty());
}

// The copy is the file's own form: its parent is kept, so a picture of its own over a parent's pixels reads as the file does.
BOOST_AUTO_TEST_CASE(TheCopyOfADocument_KeepsItsParent)
{
	write(res_root, "base_texture.desc", R"json({"__type":"texture_2d_desc","__version":1,"header":{"mag":"nearest"}})json");
	edt::texture_preview_content& shown = open_document(
		R"json({"__type":"texture_2d_desc","__version":1,"__parent":"res://base_texture.desc","data":"res://rock.png"})json");
	BOOST_REQUIRE((shown.update() == edt::preview_content::state::changed));
	const std::optional<json::object> texture = stored(shown.texture_tag());
	BOOST_REQUIRE(texture.has_value());
	BOOST_TEST(texture->at("__parent").as_string() == "res://base_texture.desc");
	BOOST_TEST(texture->at("data").as_string() == "res://rock.png");
	BOOST_TEST(!texture->contains("header"));
}

BOOST_AUTO_TEST_CASE(Release_ErasesBothCopies)
{
	edt::texture_preview_content& shown = open_document();
	BOOST_REQUIRE((shown.update() == edt::preview_content::state::changed));
	shown.release();
	BOOST_TEST(!stored(shown.texture_tag()).has_value());
	BOOST_TEST(!stored(shown.material_tag()).has_value());
	// Shown again: written again.
	BOOST_TEST((shown.update() == edt::preview_content::state::changed));
	BOOST_TEST(stored(shown.texture_tag()).has_value());
}

BOOST_AUTO_TEST_CASE(APicture_IsAChildOfTheBaseTexture_ThatNamesIt)
{
	edt::texture_preview_content picture(edt::texture_preview_source{
		.body = [] { return json::value(json::object{ { "data", "res://rock.png" } }); },
		.generation = [] { return std::uint64_t{ 0 }; },
		.parent = "res://base_texture.desc" }, services);
	BOOST_TEST((picture.update() == edt::preview_content::state::changed));
	const std::optional<json::object> texture = stored(picture.texture_tag());
	BOOST_REQUIRE(texture.has_value());
	BOOST_TEST(texture->at("__parent").as_string() == "res://base_texture.desc");
	BOOST_TEST(texture->at("data").as_string() == "res://rock.png");

	// No body: nothing to show.
	edt::texture_preview_content none(edt::texture_preview_source{}, services);
	BOOST_TEST((none.update() == edt::preview_content::state::none));
}

BOOST_AUTO_TEST_SUITE_END()
