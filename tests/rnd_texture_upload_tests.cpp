#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/desc/desc_system.h>
#include <snk/resource/adapters/res_pct_adapter.h>
#include <boost/json.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// #926: what a texture is made of comes from its decoded picture, never from the numbers its desc file gives: a type,
// a depth or a format from the file would make a driver read past the picture.
namespace {

using namespace rnd::driver;
namespace json = boost::json;

struct texture_upload_fixture : test_util::null_engine_fixture
{
	texture_upload_fixture()
		: null_engine_fixture(false)
		, render(storage.require<rnd::render_system>())
		, gpu(dynamic_cast<null::driver&>(*render.get_driver()))
		, textures(render.get_texture_manager())
	{
		gpu.observe_texture_uploads([this](texture_handle handle, const void* data, const texture_region& region) {
			const std::size_t pixel = bytes_per_pixel(gpu.info(handle).data.format);
			const auto* bytes = static_cast<const unsigned char*>(data);
			uploads.push_back({ handle, region, std::vector<unsigned char>(bytes, bytes + static_cast<std::size_t>(region.width) * region.height * pixel) });
		});
	}
	~texture_upload_fixture() { gpu.observe_texture_uploads({}); }

	static std::size_t bytes_per_pixel(texture_header::TYPE format)
	{
		switch (format) {
		case texture_header::TYPE::R8: return 1;
		case texture_header::TYPE::RGB8: return 3;
		default: return 4;
		}
	}

	res::resource_system& resources() { return storage.require<res::resource_system>(); }

	res::tag picture(const std::string& name, int width, int height, int channels, const std::vector<unsigned char>& pixels)
	{
		BOOST_REQUIRE(pixels.size() == static_cast<std::size_t>(width) * height * channels);
		const res::tag tag{ "memory://upload/" + name };
		res::raw_image_adapter::raw_image_header header{ { width, height }, channels };
		std::vector<std::byte> bytes(sizeof(header) + pixels.size());
		std::memcpy(bytes.data(), &header, sizeof(header));
		std::memcpy(bytes.data() + sizeof(header), pixels.data(), pixels.size());
		BOOST_REQUIRE(resources().store(tag, bytes));
		return tag;
	}

	res::tag description(const std::string& name, const json::object& body)
	{
		const res::tag tag{ "memory://upload/" + name };
		BOOST_REQUIRE(resources().store(tag, test_util::to_bytes(json::serialize(body))));
		return tag;
	}

	// As the frames ask for it: the desc and its pictures load in the background.
	rnd::texture_id settle(const res::tag& texture)
	{
		rnd::texture_id id = textures.require(texture);
		for (int frame = 0; frame < 10 && !textures.is_ready(id); ++frame) {
			test_util::pump(tasks);
			id = textures.require(texture);
		}
		return id;
	}

	res::tag cubemap(const std::string& name, const std::vector<res::tag>& faces)
	{
		BOOST_REQUIRE(faces.size() == 6u);
		json::object data;
		std::size_t index = 0;
		for (const char* side : { "right", "left", "bottom", "top", "front", "back" }) {
			data[side] = std::string(faces[index++].view());
		}
		return description(name, { { "__type", "texture_cubemap_desc" }, { "data", data } });
	}

	// A texture desc that holds its pixels (#1204).
	res::tag inline_texture(const std::string& name, const json::object& own, json::object header = {})
	{
		json::object body{ { "__type", "texture_2d_desc" }, { "pixels", own } };
		if (!header.empty()) body["header"] = std::move(header);
		return description(name, body);
	}

	static json::object pixels(int width, int height, std::string_view format, std::string_view base64)
	{
		return { { "width", width }, { "height", height }, { "format", format }, { "base64", base64 } };
	}

	// As desc_resave and the editor write a desc: the adapter's bytes of the loaded desc.
	std::vector<std::byte> saved(const res::tag& tag)
	{
		// Its picture loaded first: a save of a desc waits for the picture it names.
		BOOST_REQUIRE(textures.is_ready(settle(tag)));
		auto handle = resources().require_sync<rnd::texture_desc>(tag);
		test_util::pump(tasks);
		BOOST_REQUIRE(handle.is_ready());
		core::desc::desc_adapter_t adapter(resources(), storage.require<desc::desc_system>());
		return adapter.serialize(tag, handle.get());
	}

	struct upload
	{
		texture_handle handle;
		texture_region region;
		std::vector<unsigned char> pixels;
	};

	rnd::render_system& render;
	null::driver& gpu;
	rnd::texture_manager& textures;
	std::vector<upload> uploads;
};

}

BOOST_FIXTURE_TEST_SUITE(RndTextureUploadTests, texture_upload_fixture)

BOOST_AUTO_TEST_CASE(A2dTexture_TakesItsShapeFromThePicture_WhateverTheFileSays)
{
	const res::tag image = picture("shape.raw", 4, 2, 4, std::vector<unsigned char>(4 * 2 * 4, 9));
	const res::tag texture = description("shape.desc", { { "__type", "texture_2d_desc" }, { "data", std::string(image.view()) },
		{ "header", json::object{ { "type", "texture_3d" },
			{ "data", json::object{ { "format", "RGBA32F" }, { "mip_levels", 99 },
				{ "extent", json::object{ { "width", 4096 }, { "height", 4096 }, { "depth", 64 } } } } } } } });

	const rnd::texture_id id = settle(texture);

	BOOST_REQUIRE(textures.is_ready(id));
	const texture_header& made = textures.info(id);
	BOOST_TEST((made.type == TEXTURE_TYPE::TEXTURE_2D));
	BOOST_TEST((made.data.format == texture_header::TYPE::RGBA8));
	BOOST_TEST(made.data.extent.width == 4u);
	BOOST_TEST(made.data.extent.height == 2u);
	BOOST_TEST(made.data.extent.depth == 1u);
	BOOST_TEST(made.data.extent.layers == 1u);
	BOOST_TEST(made.data.extent.faces == 1u);
	// 4x2 has three levels: 4x2, 2x1, 1x1.
	BOOST_TEST(made.data.mip_levels == 3u);
}

BOOST_AUTO_TEST_CASE(APictureOfOneOrThreeChannels_KeepsItsOwnFormat)
{
	const res::tag grey = picture("grey.raw", 3, 1, 1, { 1, 2, 3 });
	const res::tag colour = picture("colour.raw", 1, 1, 3, { 4, 5, 6 });
	const rnd::texture_id grey_id = settle(description("grey.desc", { { "__type", "texture_2d_desc" }, { "data", std::string(grey.view()) } }));
	const rnd::texture_id colour_id = settle(description("colour.desc", { { "__type", "texture_2d_desc" }, { "data", std::string(colour.view()) } }));

	BOOST_REQUIRE(textures.is_ready(grey_id));
	BOOST_REQUIRE(textures.is_ready(colour_id));
	BOOST_TEST((textures.info(grey_id).data.format == texture_header::TYPE::R8));
	BOOST_TEST((textures.info(colour_id).data.format == texture_header::TYPE::RGB8));
}

BOOST_AUTO_TEST_CASE(APictureOfTwoChannels_IsUploadedAsFour)
{
	const res::tag image = picture("grey_alpha.raw", 2, 1, 2, { 10, 200, 30, 40 });
	const rnd::texture_id id = settle(description("grey_alpha.desc", { { "__type", "texture_2d_desc" }, { "data", std::string(image.view()) } }));

	BOOST_REQUIRE(textures.is_ready(id));
	BOOST_TEST((textures.info(id).data.format == texture_header::TYPE::RGBA8));
	BOOST_REQUIRE(!uploads.empty());
	BOOST_TEST((uploads.back().pixels == std::vector<unsigned char>{ 10, 10, 10, 200, 30, 30, 30, 40 }));
}

BOOST_AUTO_TEST_CASE(APictureOfFiveChannels_MakesNoTexture)
{
	const res::tag image = picture("five.raw", 1, 1, 5, { 1, 2, 3, 4, 5 });
	const rnd::texture_id id = settle(description("five.desc", { { "__type", "texture_2d_desc" }, { "data", std::string(image.view()) } }));

	BOOST_TEST(!textures.is_ready(id));
	BOOST_TEST(uploads.empty());
}

BOOST_AUTO_TEST_CASE(ACubemap_OfSixAlikeFaces_IsUploadedFaceByFace)
{
	std::vector<res::tag> faces;
	for (int face = 0; face < 6; ++face) {
		faces.push_back(picture("side" + std::to_string(face) + ".raw", 1, 1, 4, std::vector<unsigned char>(4, static_cast<unsigned char>(face + 1))));
	}

	const rnd::texture_id id = settle(cubemap("cube.desc", faces));

	BOOST_REQUIRE(textures.is_ready(id));
	BOOST_TEST((textures.info(id).type == TEXTURE_TYPE::TEXTURE_CUBE_MAP));
	BOOST_TEST(textures.info(id).data.extent.faces == 6u);
	BOOST_REQUIRE(uploads.size() == 6u);
	for (std::uint32_t face = 0; face < 6; ++face) {
		BOOST_TEST(uploads[face].region.face == face);
		BOOST_TEST((uploads[face].pixels == std::vector<unsigned char>(4, static_cast<unsigned char>(face + 1))));
	}
}

BOOST_AUTO_TEST_CASE(ACubemap_WithAFaceUnlikeTheOthers_MakesNoTexture_AndThrowsNothing)
{
	const res::tag square = picture("ok.raw", 2, 2, 4, std::vector<unsigned char>(16, 7));
	const res::tag smaller = picture("smaller.raw", 1, 1, 4, std::vector<unsigned char>(4, 7));
	const res::tag fewer_channels = picture("rgb.raw", 2, 2, 3, std::vector<unsigned char>(12, 7));
	const res::tag two_channels = picture("la.raw", 2, 2, 2, std::vector<unsigned char>(8, 7));
	const res::tag not_square = picture("wide.raw", 4, 1, 4, std::vector<unsigned char>(16, 7));
	const res::tag five_channels = picture("five_side.raw", 2, 2, 5, std::vector<unsigned char>(20, 7));

	std::size_t index = 0;
	for (const res::tag& odd : { smaller, fewer_channels, not_square, five_channels }) {
		BOOST_TEST_CONTEXT(odd.view()) {
			std::vector<res::tag> faces(6, square);
			faces[4] = odd;
			const rnd::texture_id id = settle(cubemap("odd" + std::to_string(index++) + ".desc", faces));
			BOOST_TEST(!textures.is_ready(id));
		}
	}
	BOOST_TEST(uploads.empty());

	// Grey with alpha becomes four channels, like the other faces: that one fits.
	std::vector<res::tag> faces(6, square);
	faces[4] = two_channels;
	BOOST_TEST(textures.is_ready(settle(cubemap("with_grey_alpha.desc", faces))));
}

BOOST_AUTO_TEST_CASE(AGeneratedTexture_WithAShortBufferOrNoSize_MakesNoTexture)
{
	const res::tag tag{ "memory://upload/generated" };
	BOOST_TEST(!textures.generate(tag, { 2, 2 }, texture_header::TYPE::RGB8, std::vector<unsigned char>(2 * 2 * 3 - 1)).is_valid());
	BOOST_TEST(!textures.generate(tag, { 0, 2 }, texture_header::TYPE::RGBA8, std::vector<unsigned char>(16)).is_valid());
	BOOST_TEST(!textures.generate(tag, { 2, -1 }, texture_header::TYPE::RGBA8, std::vector<unsigned char>(16)).is_valid());
	BOOST_TEST(!textures.generate(tag, { 2, 2 }, texture_header::TYPE::RGBA16F, std::vector<unsigned char>(2 * 2 * 16)).is_valid());
	BOOST_TEST(uploads.empty());

	const rnd::texture_id id = textures.generate(tag, { 2, 2 }, texture_header::TYPE::RGB8, std::vector<unsigned char>(2 * 2 * 3));
	BOOST_TEST(textures.is_ready(id));
}

// #1204: a texture whose desc holds its pixels; its shape comes from them alone, as a picture's does (#926).
BOOST_AUTO_TEST_CASE(ASolidColour_OfInlinePixels_IsUploadedAsItsBytes_WhateverTheHeaderSays)
{
	const res::tag texture = inline_texture("red.desc", pixels(1, 1, "RGBA8", "/wAA/w=="),
		{ { "type", "texture_3d" }, { "usage", "color_target" },
			{ "data", json::object{ { "format", "RGBA32F" }, { "extent", json::object{ { "width", 64 }, { "height", 64 }, { "depth", 8 } } } } } });

	const rnd::texture_id id = settle(texture);

	BOOST_REQUIRE(textures.is_ready(id));
	const texture_header& made = textures.info(id);
	BOOST_TEST((made.type == TEXTURE_TYPE::TEXTURE_2D));
	BOOST_TEST((made.usage == texture_usage::sampled));
	BOOST_TEST((made.data.format == texture_header::TYPE::RGBA8));
	BOOST_TEST(made.data.extent.width == 1u);
	BOOST_TEST(made.data.extent.height == 1u);
	BOOST_TEST(made.data.extent.depth == 1u);
	BOOST_REQUIRE(uploads.size() == 1u);
	BOOST_TEST((uploads.front().pixels == std::vector<unsigned char>{ 255, 0, 0, 255 }));
}

BOOST_AUTO_TEST_CASE(ACheckerboard_OfInlinePixels_KeepsTheSamplingOfItsHeader)
{
	// 2x2 RGB8: white, black / black, white.
	const res::tag texture = inline_texture("checker.desc", pixels(2, 2, "RGB8", "////AAAAAAAA////"),
		{ { "wrap", "repeat" }, { "min", "nearest" }, { "mag", "nearest" } });

	const rnd::texture_id id = settle(texture);

	BOOST_REQUIRE(textures.is_ready(id));
	const texture_header& made = textures.info(id);
	BOOST_TEST((made.data.format == texture_header::TYPE::RGB8));
	BOOST_TEST((made.wrap == texture_header::WRAPPING::REPEAT));
	BOOST_TEST((made.min == texture_header::FILTERING::NEAREST));
	BOOST_TEST((made.mag == texture_header::FILTERING::NEAREST));
	BOOST_REQUIRE(uploads.size() == 1u);
	// Rows of 6 bytes (no 4-byte alignment), the bottom row first as a decoded picture's.
	BOOST_TEST((uploads.front().pixels == std::vector<unsigned char>{ 0, 0, 0, 255, 255, 255, 255, 255, 255, 0, 0, 0 }));
}

BOOST_AUTO_TEST_CASE(InlinePixels_OfOneChannel_ClampTheirMipLevelsToTheChain)
{
	const res::tag texture = inline_texture("grey.desc", pixels(4, 4, "R8", "AAECAwQFBgcICQoLDA0ODw=="),
		{ { "data", json::object{ { "mip_levels", 8 } } } });

	const rnd::texture_id id = settle(texture);

	BOOST_REQUIRE(textures.is_ready(id));
	BOOST_TEST((textures.info(id).data.format == texture_header::TYPE::R8));
	// 4x4 has three levels: 4x4, 2x2, 1x1.
	BOOST_TEST(textures.info(id).data.mip_levels == 3u);
	BOOST_REQUIRE(!uploads.empty());
	BOOST_TEST(uploads.front().pixels.size() == 16u);
	BOOST_TEST(uploads.front().pixels[0] == 12);
	BOOST_TEST(uploads.front().pixels[15] == 3);
}

// A texture_2d_desc is two-dimensional: no type in its header loads 1D, 3D or array pixels from it (US-27-3 has those).
BOOST_AUTO_TEST_CASE(InlinePixels_UnderA1dOr2dArrayType_StillMakeA2dTexture)
{
	for (const char* type : { "texture_1d", "texture_2d_array", "texture_1d_array" }) {
		BOOST_TEST_CONTEXT(type) {
			const rnd::texture_id id = settle(inline_texture(std::string(type) + ".desc", pixels(2, 1, "RGBA8", "AQIDBAUGBwg="),
				{ { "type", type }, { "data", json::object{ { "extent", json::object{ { "width", 2 }, { "height", 1 }, { "depth", 3 } } } } } }));
			BOOST_REQUIRE(textures.is_ready(id));
			BOOST_TEST((textures.info(id).type == TEXTURE_TYPE::TEXTURE_2D));
			BOOST_TEST(textures.info(id).data.extent.layers == 1u);
			BOOST_TEST(textures.info(id).data.extent.depth == 1u);
		}
	}
}

BOOST_AUTO_TEST_CASE(AChangedDescOfInlinePixels_ReloadsTheTextureWithTheNewBytes)
{
	const res::tag texture = inline_texture("changing.desc", pixels(1, 1, "RGBA8", "/wAA/w=="));
	BOOST_REQUIRE(textures.is_ready(settle(texture)));
	BOOST_REQUIRE(uploads.size() == 1u);

	inline_texture("changing.desc", pixels(1, 1, "RGBA8", "AP8A/w=="));
	resources().signal_changed(texture);
	for (int frame = 0; frame < 10 && uploads.size() < 2; ++frame) {
		test_util::pump(tasks);
		textures.require(texture);
	}

	BOOST_REQUIRE(uploads.size() == 2u);
	BOOST_TEST((uploads.back().pixels == std::vector<unsigned char>{ 0, 255, 0, 255 }));
}

// The rows of a desc are read as a PNG's: top row first. The same picture as a PNG file and as inline pixels gives the
// driver the same bytes (red on top, blue below; uploaded bottom row first). No colour conversion either way: RGBA8 is
// uploaded as it is, as a PNG's pixels are (an sRGB format is #1371).
BOOST_AUTO_TEST_CASE(InlinePixels_AreUploadedAsTheSamePictureAsAPngFile)
{
	// 1x2 RGBA8, red top row, blue bottom row.
	const std::vector<unsigned char> png{ 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
		0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x99, 0x81, 0xb6, 0x27, 0x00, 0x00, 0x00, 0x11, 0x49,
		0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0x00, 0x44, 0x0c, 0xff, 0xff, 0x03, 0x00, 0x11, 0xf8, 0x03, 0xfd, 0x08, 0xed,
		0xb1, 0x56, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82 };
	const res::tag file{ "memory://upload/red_over_blue.png" };
	std::vector<std::byte> bytes(png.size());
	std::memcpy(bytes.data(), png.data(), png.size());
	BOOST_REQUIRE(resources().store(file, bytes));

	BOOST_REQUIRE(textures.is_ready(settle(description("from_png.desc", { { "__type", "texture_2d_desc" }, { "data", std::string(file.view()) } }))));
	BOOST_REQUIRE(uploads.size() == 1u);
	BOOST_REQUIRE(textures.is_ready(settle(inline_texture("from_pixels.desc", pixels(1, 2, "RGBA8", "/wAA/wAA//8=")))));
	BOOST_REQUIRE(uploads.size() == 2u);

	const std::vector<unsigned char> bottom_first{ 0, 0, 255, 255, 255, 0, 0, 255 };
	BOOST_TEST((uploads[0].pixels == bottom_first));
	BOOST_TEST((uploads[1].pixels == bottom_first));
}

// What is saved reads back to the same bytes: a desc without pixels gets no "pixels" key (byte-stable resave), one with
// pixels keeps them exactly.
BOOST_AUTO_TEST_CASE(ADescWithOrWithoutInlinePixels_SavesToTheBytesItWasReadFrom)
{
	const res::tag image = picture("saved.raw", 1, 1, 4, { 1, 2, 3, 4 });
	const res::tag by_picture = description("by_picture.desc", { { "__type", "texture_2d_desc" }, { "data", std::string(image.view()) },
		{ "header", json::object{ { "wrap", "repeat" } } } });
	const res::tag by_pixels = inline_texture("by_pixels.desc", pixels(2, 2, "RGB8", "////AAAAAAAA////"), { { "wrap", "repeat" } });

	for (const res::tag& tag : { by_picture, by_pixels }) {
		BOOST_TEST_CONTEXT(tag.view()) {
			const std::vector<std::byte> first = saved(tag);
			const res::tag again{ std::string(tag.view()) + ".again.desc" };
			BOOST_REQUIRE(resources().store(again, first));
			BOOST_TEST((saved(again) == first));

			const json::object written = json::parse(std::string(reinterpret_cast<const char*>(first.data()), first.size())).as_object();
			BOOST_TEST(written.contains("pixels") == (tag == by_pixels));
			BOOST_TEST(written.contains("data") == (tag == by_picture));
			if (tag == by_pixels) {
				BOOST_TEST((written.at("pixels") == json::value(pixels(2, 2, "RGB8", "////AAAAAAAA////"))));
			}
		}
	}
}

// A child's source replaces its parent's: `pixels` a parent's picture, `data` a parent's pixels, through three levels;
// a child that names neither takes its parent's. Each saves over its parent and reads back the same.
BOOST_AUTO_TEST_CASE(AChildDesc_TakesItsOwnSource_OverItsParents)
{
	const res::tag image = picture("parent.raw", 1, 1, 4, { 9, 9, 9, 9 });
	const res::tag picture_parent = description("picture_parent.desc", { { "__type", "texture_2d_desc" }, { "data", std::string(image.view()) } });
	const res::tag pixels_parent = inline_texture("pixels_parent.desc", pixels(1, 1, "RGBA8", "AQIDBA=="));

	const rnd::texture_id with_pixels = settle(description("pixels_child.desc", { { "__type", "texture_2d_desc" },
		{ "__parent", std::string(picture_parent.view()) }, { "pixels", pixels(1, 1, "R8", "Bw==") } }));
	const rnd::texture_id with_picture = settle(description("picture_child.desc", { { "__type", "texture_2d_desc" },
		{ "__parent", std::string(pixels_parent.view()) }, { "data", std::string(image.view()) } }));

	BOOST_REQUIRE(textures.is_ready(with_pixels));
	BOOST_REQUIRE(textures.is_ready(with_picture));
	BOOST_TEST((textures.info(with_pixels).data.format == texture_header::TYPE::R8));
	BOOST_TEST((textures.info(with_picture).data.format == texture_header::TYPE::RGBA8));
	BOOST_TEST(textures.info(with_picture).data.extent.width == 1u);

	// picture -> pixels -> (nothing) and picture -> pixels -> picture.
	const res::tag middle = description("middle.desc", { { "__type", "texture_2d_desc" }, { "__parent", std::string(picture_parent.view()) },
		{ "pixels", pixels(2, 1, "R8", "BQY=") } });
	const res::tag inherits = description("inherits.desc", { { "__type", "texture_2d_desc" }, { "__parent", std::string(middle.view()) },
		{ "header", json::object{ { "wrap", "repeat" } } } });
	const res::tag back_to_picture = description("back_to_picture.desc", { { "__type", "texture_2d_desc" },
		{ "__parent", std::string(middle.view()) }, { "data", std::string(image.view()) } });
	// A file with its own pixels over a parent's: the nested patch form ({"pixels": {"base64": ...}}) does not read back,
	// so the save writes the pixels whole and keeps __parent.
	const res::tag own_pixels = description("own_pixels.desc", { { "__type", "texture_2d_desc" }, { "__parent", std::string(middle.view()) },
		{ "pixels", pixels(2, 1, "R8", "CQo=") } });
	// A null takes the parent's pixels away: no source, no texture (as a desc without "data" before #1204).
	const res::tag no_source = description("no_source.desc", { { "__type", "texture_2d_desc" }, { "__parent", std::string(middle.view()) },
		{ "pixels", nullptr } });
	BOOST_TEST(!textures.is_ready(settle(no_source)));
	BOOST_TEST(uploads.size() == 2u);

	const rnd::texture_id inherited = settle(inherits);
	const rnd::texture_id replaced = settle(back_to_picture);
	BOOST_REQUIRE(textures.is_ready(inherited));
	BOOST_REQUIRE(textures.is_ready(replaced));
	BOOST_TEST((textures.info(inherited).data.format == texture_header::TYPE::R8));
	BOOST_TEST(textures.info(inherited).data.extent.width == 2u);
	BOOST_TEST((textures.info(replaced).data.format == texture_header::TYPE::RGBA8));

	BOOST_REQUIRE(textures.is_ready(settle(own_pixels)));
	BOOST_TEST((uploads.back().pixels == std::vector<unsigned char>{ 9, 10 }));

	for (const res::tag& tag : { middle, inherits, back_to_picture, own_pixels }) {
		BOOST_TEST_CONTEXT(tag.view()) {
			const std::vector<std::byte> first = saved(tag);
			const json::object written = json::parse(std::string(reinterpret_cast<const char*>(first.data()), first.size())).as_object();
			BOOST_TEST(written.contains("__parent"));
			const res::tag again{ std::string(tag.view()) + ".again.desc" };
			BOOST_REQUIRE(resources().store(again, first));
			BOOST_TEST((saved(again) == first));
		}
	}
}

BOOST_AUTO_TEST_SUITE_END()
