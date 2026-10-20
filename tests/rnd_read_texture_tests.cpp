#include <boost/test/unit_test.hpp>
#include "rnd_null_driver.h"
#include "rnd_null_swapchain.h"
#include "rnd_validation_driver.h"
#include <snk/driver/rnd_texture_owner.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// driver_interface::read_texture: the null backend answers with a placeholder that tells which part of the
// texture was asked for, the validation decorator checks the call and forwards it. The GL driver is covered
// by the capture step of the editor visual smoke.

namespace {

using namespace rnd::driver;
namespace vl = rnd::driver::validation;

texture_header color_texture(uint32_t width, uint32_t height, texture_header::TYPE format = texture_header::TYPE::RGBA8)
{
	texture_header header;
	header.data.extent.width = width;
	header.data.extent.height = height;
	header.data.format = format;
	header.usage = texture_usage::color_target | texture_usage::sampled;
	return header;
}

texture_header depth_texture(uint32_t width, uint32_t height)
{
	texture_header header = color_texture(width, height, texture_header::TYPE::D24_S8);
	header.usage = texture_usage::depth_target;
	return header;
}

int at(const std::vector<std::byte>& bytes, std::size_t index)
{
	return std::to_integer<int>(bytes.at(index));
}

}

BOOST_AUTO_TEST_SUITE(RndReadTextureTests)

BOOST_AUTO_TEST_CASE(Null_ReadsThePlaceholderOfTheRect)
{
	null::driver drv;
	auto texture = make_texture(drv, color_texture(8, 4));

	const auto bytes = drv.read_texture(texture->handle(), { 2, 1, 3, 2 });

	BOOST_REQUIRE_EQUAL(bytes.size(), 3u * 2u * 4u);
	// First row of the rect: pixels (2,1) (3,1) (4,1); then (2,2).
	BOOST_CHECK_EQUAL(at(bytes, 0), 2);
	BOOST_CHECK_EQUAL(at(bytes, 1), 1);
	BOOST_CHECK_EQUAL(at(bytes, 2), 0);
	BOOST_CHECK_EQUAL(at(bytes, 3), 255);
	BOOST_CHECK_EQUAL(at(bytes, 8), 4);
	BOOST_CHECK_EQUAL(at(bytes, 9), 1);
	BOOST_CHECK_EQUAL(at(bytes, 12), 2);
	BOOST_CHECK_EQUAL(at(bytes, 13), 2);
}

BOOST_AUTO_TEST_CASE(Null_ReadsTheWholeTexture)
{
	null::driver drv;
	auto texture = make_texture(drv, color_texture(5, 3, texture_header::TYPE::RGBA16F));

	const auto bytes = drv.read_texture(texture->handle(), { 0, 0, 5, 3 });

	BOOST_CHECK_EQUAL(bytes.size(), 5u * 3u * 4u);
}

BOOST_AUTO_TEST_CASE(Null_ReadsNothingFromWhatCannotBeRead)
{
	null::driver drv;
	auto texture = make_texture(drv, color_texture(8, 4));
	BOOST_CHECK(drv.read_texture(texture->handle(), { 6, 0, 3, 1 }).empty());	// past the right edge
	BOOST_CHECK(drv.read_texture(texture->handle(), { 0, 3, 1, 2 }).empty());	// past the top edge
	BOOST_CHECK(drv.read_texture(texture->handle(), { -1, 0, 2, 2 }).empty());
	BOOST_CHECK(drv.read_texture(texture->handle(), { 0, 0, 0, 2 }).empty());
	BOOST_CHECK(drv.read_texture(texture->handle(), { 0, 0, 2, 0 }).empty());

	auto depth = make_texture(drv, depth_texture(8, 4));
	BOOST_CHECK(drv.read_texture(depth->handle(), { 0, 0, 8, 4 }).empty());

	texture_header cube = color_texture(8, 8);
	cube.type = TEXTURE_TYPE::TEXTURE_CUBE_MAP;
	auto cube_texture = make_texture(drv, cube);
	BOOST_CHECK(drv.read_texture(cube_texture->handle(), { 0, 0, 8, 8 }).empty());
}

struct validation_fixture
{
	validation_fixture()
		: chain(std::make_unique<null::swapchain>())
		, drv(chain.create_driver())
	{
	}

	vl::swapchain chain;
	std::unique_ptr<driver_interface> drv;
};

BOOST_FIXTURE_TEST_CASE(Validation_ForwardsAValidRead, validation_fixture)
{
	auto texture = make_texture(*drv, color_texture(8, 4));

	const auto bytes = drv->read_texture(texture->handle(), { 1, 1, 2, 2 });

	BOOST_REQUIRE_EQUAL(bytes.size(), 2u * 2u * 4u);
	BOOST_CHECK_EQUAL(at(bytes, 0), 1);
	BOOST_CHECK_EQUAL(at(bytes, 1), 1);
	BOOST_CHECK_EQUAL(chain.state().total(), 0u);
}

BOOST_FIXTURE_TEST_CASE(Validation_ReportsAReadOutsideTheTexture, validation_fixture)
{
	auto texture = make_texture(*drv, color_texture(8, 4));

	BOOST_CHECK(drv->read_texture(texture->handle(), { 4, 0, 8, 4 }).empty());

	BOOST_CHECK_EQUAL(chain.state().count(vl::rule::invalid_read), 1u);
	BOOST_CHECK_EQUAL(chain.state().total(), 1u);
}

BOOST_FIXTURE_TEST_CASE(Validation_ReportsAReadOfADepthTexture, validation_fixture)
{
	auto depth = make_texture(*drv, depth_texture(8, 4));

	BOOST_CHECK(drv->read_texture(depth->handle(), { 0, 0, 8, 4 }).empty());

	BOOST_CHECK_EQUAL(chain.state().count(vl::rule::invalid_read), 1u);
}

BOOST_AUTO_TEST_CASE(Validation_RuleHasAName)
{
	BOOST_TEST(std::string(vl::rule_name(vl::rule::invalid_read)) == "invalid_read");
}

BOOST_AUTO_TEST_SUITE_END()
