#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/render/texture/rnd_texture_desc.h>
#include <snk/resource/resources/res_resource_picture.h>
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

	static std::string geometry(std::string_view vertices, std::string_view bounds = "[]")
	{
		return std::string(R"({"__type":"geometry_desc","layout":{"position":"vec3"},"indices":[0,1,2],"vertices":)") + std::string(vertices)
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

BOOST_AUTO_TEST_SUITE_END()
