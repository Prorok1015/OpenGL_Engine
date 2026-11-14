#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/texture/rnd_texture_manager.h>
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
	BOOST_TEST(uploads.empty());

	const rnd::texture_id id = textures.generate(tag, { 2, 2 }, texture_header::TYPE::RGB8, std::vector<unsigned char>(2 * 2 * 3));
	BOOST_TEST(textures.is_ready(id));
}

BOOST_AUTO_TEST_SUITE_END()
