#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/render/texture/rnd_texture_desc.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/resource/resources/res_resource_picture.h>
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// #930: a value a desc file holds that this build cannot read fails the load of that file. It must not assert (an abort in
// Debug) and must not go on with a value nobody wrote.
namespace {

struct data_error_fixture : test_util::null_engine_fixture
{
	data_error_fixture() : null_engine_fixture(false) {}

	res::resource_system& resources() { return storage.require<res::resource_system>(); }

	template<class T>
	res::res_handle<T> load(std::string_view name, std::string_view text)
	{
		std::string path = "memory://bad_data/";
		path.append(name);
		const res::tag tag{ path };
		BOOST_REQUIRE(resources().store(tag, test_util::to_bytes(text)));
		auto handle = resources().require_sync<T>(tag);
		test_util::pump(tasks);
		return handle;
	}

	// The json_path of the load's own diagnostic (empty: none).
	template<class T>
	static std::string path_of(const res::res_handle<T>& handle)
	{
		res::diagnostics_collector collector;
		handle.collect_into(collector);
		const auto& items = collector.items();
		return items.empty() ? std::string{} : items.front().json_path;
	}

	// A texture_2d_desc whose "pixels" are `pixels` (raw JSON), and `extra` more members.
	static std::string inline_texture(std::string_view pixels, std::string_view extra = "")
	{
		return std::string(R"({"__type":"texture_2d_desc",)") + std::string(extra) + R"("pixels":)" + std::string(pixels) + "}";
	}

	// One-byte vertices and no indices: any bytes pass the data checks of arrays (#1068), only their JSON is tested here.
	static std::string geometry(std::string_view vertices, std::string_view bounds = "[]")
	{
		return std::string(R"({"__type":"geometry_desc","layout":{"id":"bool"},"indices":[],"vertices":)") + std::string(vertices)
			+ R"(,"bounds":)" + std::string(bounds) + "}";
	}
};

}

BOOST_FIXTURE_TEST_SUITE(RndDescDataErrorsTests, data_error_fixture)

BOOST_AUTO_TEST_CASE(ATextureOfAnUnknownType_FailsItsLoad_AndNamesTheValue)
{
	const auto texture = load<rnd::texture_desc>("type.desc", R"({"__type":"texture_2d_desc","header":{"type":"texture_9d"}})");

	BOOST_TEST(texture.has_error());
	BOOST_TEST(texture.error_text().find("unknown texture type 'texture_9d'") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ATextureOfAnUnknownFormat_FailsItsLoad_AndNamesTheValue)
{
	const auto texture = load<rnd::texture_desc>("format.desc",
		R"({"__type":"texture_2d_desc","header":{"type":"texture_2d","data":{"format":"RGBA99\nline two"}}})");

	BOOST_TEST(texture.has_error());
	BOOST_TEST(texture.error_text().find("unknown texture format 'RGBA99\\nline two'") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AnUnknownValue_WithABidiOverrideAndACutMultiByteChar_IsShownWithoutRawControlBytes)
{
	// "ab\\u{202E}cd" (12 bytes) + 48 'x' = 60: the 2-byte char that follows would end past the 61 bytes that leave room
	// for "...", so the cut drops it whole.
	const std::string name = "ab\\u202Ecd" + std::string(48, 'x') + "\\u00e9zzzz";
	const auto texture = load<rnd::texture_desc>("hidden.desc",
		R"({"__type":"texture_2d_desc","header":{"type":")" + name + R"("}})");

	BOOST_TEST(texture.has_error());
	const std::string text = texture.error_text();
	BOOST_TEST(text.find("unknown texture type 'ab\\u{202E}cd" + std::string(48, 'x') + "...'") != std::string::npos);
	BOOST_TEST(text.find("\xE2\x80\xAE") == std::string::npos);
	BOOST_TEST(text.find('\xC3') == std::string::npos);
}

BOOST_AUTO_TEST_CASE(ATextureArrayType_SurvivesAWriteAndARead)
{
	for (const char* name : { "texture_1d_array", "texture_2d_array" }) {
		BOOST_TEST_CONTEXT(name) {
			const std::string text = std::string(R"({"__type":"texture_2d_desc","header":{"type":")") + name + R"("}})";
			const auto loaded = load<rnd::texture_desc>(std::string(name) + ".desc", text);

			BOOST_REQUIRE_MESSAGE(loaded.is_ready() && !loaded.has_error(), loaded.error_text());
			const auto desc = loaded.get_sync();
			BOOST_REQUIRE(desc);
			const bool is_1d = std::string_view(name) == "texture_1d_array";
			BOOST_TEST((desc->header.type == (is_1d ? rnd::driver::TEXTURE_TYPE::TEXTURE_1D_ARRAY
				: rnd::driver::TEXTURE_TYPE::TEXTURE_2D_ARRAY)));

			boost::json::object written;
			desc->serialize(written);
			BOOST_TEST(std::string_view(written.at("header").as_object().at("type").as_string()) == std::string_view(name));
		}
	}
}

BOOST_AUTO_TEST_CASE(GeometryBytes_ThatAreNoBytes_FailTheLoad)
{
	std::size_t index = 0;
	for (const char* vertices : { "[0, 255, 256]", "[1, -1]", "[1.5]", "[\"7\"]", "[null]" }) {
		BOOST_TEST_CONTEXT(vertices) {
			const std::string name = "bytes" + std::to_string(index++) + ".desc";
			const auto geometry_handle = load<rnd::geometry_desc>(name, geometry(vertices));
			BOOST_TEST(geometry_handle.has_error());
			BOOST_TEST(geometry_handle.error_text().find("not an integer from 0 to 255") != std::string::npos);
		}
	}
}

BOOST_AUTO_TEST_CASE(GeometryBytes_FromZeroTo255_AreRead)
{
	const auto loaded = load<rnd::geometry_desc>("good.desc", geometry("[0, 1, 254, 255]",
		R"([[[{"x":1,"y":2},{"x":3,"y":4}], 7]])"));

	BOOST_REQUIRE_MESSAGE(loaded.is_ready() && !loaded.has_error(), loaded.error_text());
	const auto desc = loaded.get_sync();
	BOOST_REQUIRE(desc);
	BOOST_TEST((desc->vertices == std::vector<std::byte>{ std::byte{ 0 }, std::byte{ 1 }, std::byte{ 254 }, std::byte{ 255 } }));
	BOOST_REQUIRE(desc->bounds.size() == 1u);
	BOOST_TEST(desc->bounds[0].first.max.x == 3.f);
	BOOST_TEST(desc->bounds[0].second == 7u);
}

// #1022: the vertices are read in one loop; a model's worth of them gives the same bytes in the same order.
BOOST_AUTO_TEST_CASE(ManyGeometryBytes_AreReadInOrder)
{
	std::vector<std::byte> expected;
	std::string vertices = "[";
	for (std::size_t i = 0; i < 100000; ++i) {
		const unsigned value = static_cast<unsigned>((i * 37u) % 256u);
		expected.push_back(std::byte(value));
		if (i != 0) vertices += ',';
		vertices += std::to_string(value);
	}
	vertices += "]";
	const auto loaded = load<rnd::geometry_desc>("many.desc", geometry(vertices));

	BOOST_REQUIRE_MESSAGE(loaded.is_ready() && !loaded.has_error(), loaded.error_text());
	const auto desc = loaded.get_sync();
	BOOST_REQUIRE(desc);
	BOOST_TEST((desc->vertices == expected));
}

// A bad byte at the end of a long array fails the load as one at the start does.
BOOST_AUTO_TEST_CASE(ABadByteAfterManyGoodOnes_FailsTheLoad)
{
	std::string vertices = "[";
	for (std::size_t i = 0; i < 10000; ++i) vertices += "1,";
	vertices += "256]";
	const auto loaded = load<rnd::geometry_desc>("late.desc", geometry(vertices));
	BOOST_TEST(loaded.has_error());
	BOOST_TEST(loaded.error_text().find("not an integer from 0 to 255") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ABoundingBox_ThatIsNoPairOfPoints_FailsTheLoad)
{
	const char* boxes[] = {
		R"([[[{"x":1,"y":2}], 7]])",
		R"([[[{"x":1,"y":2},{"x":3,"y":4},{"x":5,"y":6}], 7]])",
		R"([[[{"x":1,"y":2},{"x":3}], 7]])",
		R"([[[{"x":1,"y":2},"point"], 7]])",
		R"([[{"x":1,"y":2}, 7]])",
	};
	std::size_t index = 0;
	for (const char* bounds : boxes) {
		BOOST_TEST_CONTEXT(bounds) {
			const std::string name = "box" + std::to_string(index++) + ".desc";
			const auto geometry_handle = load<rnd::geometry_desc>(name, geometry("[1]", bounds));
			BOOST_TEST(geometry_handle.has_error());
			BOOST_TEST(geometry_handle.error_text().find("bounding box") != std::string::npos);
		}
	}
}

BOOST_AUTO_TEST_CASE(ARawImageShorterThanItsHeader_FailsItsLoad)
{
	const auto picture = load<res::picture_resource>("short.raw", "abc");

	BOOST_TEST(picture.has_error());
	BOOST_TEST(picture.error_text().find("shorter than its header") != std::string::npos);
}

// #1204: inline pixels that do not describe themselves exactly fail the load and name their field.
BOOST_AUTO_TEST_CASE(InlinePixels_ThatAreWrong_FailTheLoad_AndNameTheField)
{
	struct bad
	{
		const char* name;
		std::string text;
		const char* path;
		const char* reason;
	};
	const bad cases[] = {
		{ "both", inline_texture(R"({"width":1,"height":1,"format":"RGBA8","base64":"/wAA/w=="})", R"("data":"memory://bad_data/x.raw",)"),
			"/pixels", "either 'data' (a picture) or 'pixels', not both" },
		{ "not_object", inline_texture(R"("/wAA/w==")"), "/pixels", "an object {width, height, format, base64}" },
		{ "no_width", inline_texture(R"({"height":1,"format":"RGBA8","base64":"/wAA/w=="})"), "/pixels/width", "need 'width'" },
		{ "zero_width", inline_texture(R"({"width":0,"height":1,"format":"RGBA8","base64":""})"), "/pixels/width", "'width' is 0: 1 to 4096" },
		{ "wide", inline_texture(R"({"width":5000,"height":1,"format":"R8","base64":""})"), "/pixels/width", "'width' is 5000: 1 to 4096" },
		{ "fraction", inline_texture(R"({"width":1.5,"height":1,"format":"R8","base64":""})"), "/pixels/width", "not a whole number" },
		{ "format", inline_texture(R"({"width":1,"height":1,"format":"RGBA16F","base64":"AAAAAAAAAAA="})"), "/pixels/format",
			"unknown inline pixel format 'RGBA16F'" },
		{ "big", inline_texture(R"({"width":256,"height":256,"format":"RGBA8","base64":""})"), "/pixels",
			"262144 bytes: inline pixels are at most 65536 bytes" },
		{ "short", inline_texture(R"({"width":1,"height":1,"format":"RGBA8","base64":"/wAA"})"), "/pixels/base64",
			"'base64' has 4 characters: 1x1 RGBA8 needs 4 bytes, 8 characters" },
		{ "long", inline_texture(R"({"width":1,"height":1,"format":"R8","base64":"/wAA/w=="})"), "/pixels/base64",
			"'base64' has 8 characters: 1x1 R8 needs 1 bytes, 4 characters" },
		{ "alphabet", inline_texture(R"({"width":1,"height":1,"format":"RGBA8","base64":"/wA*/w=="})"), "/pixels/base64", "canonical padded" },
		{ "unpadded_length", inline_texture(R"({"width":1,"height":1,"format":"RGB8","base64":"/wA="})"), "/pixels/base64", "canonical padded" },
		// The same bytes as "/w==" with the unused bits set: they would not survive a save.
		{ "noncanonical", inline_texture(R"({"width":1,"height":1,"format":"R8","base64":"/x=="})"), "/pixels/base64", "canonical padded" },
		{ "space", inline_texture(R"({"width":1,"height":1,"format":"RGB8","base64":"/wA "})"), "/pixels/base64", "canonical padded" },
		{ "unknown_key", inline_texture(R"({"width":1,"height":1,"format":"R8","base64":"/w==","colour":1})"), "/pixels/colour",
			"unknown key 'colour' in inline pixels" },
		{ "float_width", inline_texture(R"({"width":1.0,"height":1,"format":"R8","base64":"/w=="})"), "/pixels/width", "not a whole number" },
		{ "string_width", inline_texture(R"({"width":"1","height":1,"format":"R8","base64":"/w=="})"), "/pixels/width", "not a whole number" },
		{ "huge_width", inline_texture(R"({"width":18446744073709551615,"height":1,"format":"R8","base64":"/w=="})"), "/pixels/width",
			"'width' is 18446744073709551615: 1 to 4096" },
		{ "negative_height", inline_texture(R"({"width":1,"height":-1,"format":"R8","base64":"/w=="})"), "/pixels/height", "'height' is -1: 1 to 4096" },
		// The text's length is checked first: no width, but a text longer than any inline texture.
		{ "long_text", inline_texture(R"({"base64":")" + std::string(87388, 'A') + R"("})"), "/pixels/base64",
			"at most 65536 bytes (87384 characters)" },
	};
	for (const bad& item : cases) {
		BOOST_TEST_CONTEXT(item.name) {
			const auto texture = load<rnd::texture_desc>(std::string(item.name) + ".desc", item.text);
			BOOST_TEST(texture.has_error());
			BOOST_TEST(texture.error_text().find(item.reason) != std::string::npos, texture.error_text());
			BOOST_TEST(path_of(texture) == item.path);
		}
	}
}

BOOST_AUTO_TEST_CASE(InlinePixels_ThatAreRight_Load)
{
	const auto texture = load<rnd::texture_desc>("right.desc", inline_texture(R"({"width":2,"height":1,"format":"RGB8","base64":"AQIDBAUG"})"));

	BOOST_TEST(texture.is_ready(), texture.error_text());
}

BOOST_AUTO_TEST_SUITE_END()
